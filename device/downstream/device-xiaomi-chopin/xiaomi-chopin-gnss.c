// SPDX-License-Identifier: MIT
/*
 * Serve NMEA from MediaTek's stock mnld to geoclue over a unix stream socket.
 *
 * mnld is driven over the datagram protocol of the stock gps.default.so HAL:
 * requests go to the abstract socket "mtk_hal2mnl" as little-endian
 * {int32 version = 1, int32 type, payload}; reports arrive on the abstract
 * socket "mtk_mnl2hal". The receiver runs only while a client is connected
 * and stops STOP_DELAY seconds after the last one disconnects.
 *
 * usage: xiaomi-chopin-gnss SOCKET
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define STOP_DELAY 10
#define HAL_INIT 101
#define HAL_START 102
#define HAL_STOP 103
#define HAL_INJECT_TIME 105
#define MNL_REBOOT 201
#define MNL_NMEA 104

static int reports, requests;
static int clients[MAX_CLIENTS], nclients;
static double started = -1, stop_at = -1;
static int fixed;

static double now(clockid_t clock)
{
	struct timespec ts;

	clock_gettime(clock, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static socklen_t abstract_addr(struct sockaddr_un *addr, const char *name)
{
	/* The HAL pads abstract names with NULs to the full sun_path. */
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	strncpy(addr->sun_path + 1, name, sizeof(addr->sun_path) - 2);
	return sizeof(*addr);
}

static void request(int32_t type, const void *payload, size_t len)
{
	struct sockaddr_un addr;
	unsigned char msg[64];
	int32_t header[2] = { 1, type };

	memcpy(msg, header, sizeof(header));
	memcpy(msg + sizeof(header), payload, len);
	if (sendto(requests, msg, sizeof(header) + len, 0, (struct sockaddr *)&addr,
		   abstract_addr(&addr, "mtk_hal2mnl")) < 0)
		say("mnld request %d failed: %s", type, strerror(errno));
}

static void start_receiver(void)
{
	float accuracy = 1.4f;	/* gps_start() argument of the stock HAL */
	struct timex tx = { 0 };

	request(HAL_INIT, NULL, 0);
	usleep(500000);
	request(HAL_START, &accuracy, sizeof(accuracy));

	/* Only inject the time when it is NTP-synchronized. */
	if (adjtimex(&tx) != TIME_ERROR) {
		unsigned char payload[20];
		int64_t utc_ms = now(CLOCK_REALTIME) * 1000;
		int64_t boot_ms = now(CLOCK_BOOTTIME) * 1000;
		int32_t uncertainty_ms = 1000;

		memcpy(payload, &utc_ms, 8);
		memcpy(payload + 8, &boot_ms, 8);
		memcpy(payload + 16, &uncertainty_ms, 4);
		request(HAL_INJECT_TIME, payload, sizeof(payload));
	}
	started = now(CLOCK_MONOTONIC);
	fixed = 0;
	say("receiver started");
}

static void stop_receiver(void)
{
	request(HAL_STOP, NULL, 0);
	started = -1;
	say("receiver stopped");
}

static void drop_client(int i)
{
	close(clients[i]);
	clients[i] = clients[--nclients];
	if (!nclients)
		stop_at = now(CLOCK_MONOTONIC) + STOP_DELAY;
}

/* Log the time to first fix of each session, without the position. */
static void track_fix(const char *sentence, size_t len)
{
	char copy[128], *field[10];
	int n = 0;

	if (fixed || started < 0 || len >= sizeof(copy) || strncmp(sentence + 3, "GGA,", 4))
		return;
	memcpy(copy, sentence, len);
	copy[len] = '\0';
	for (char *p = copy, *f; n < 10 && (f = strsep(&p, ",*")); )
		field[n++] = f;
	if (n > 8 && *field[6] && strcmp(field[6], "0")) {
		fixed = 1;
		say("first fix after %.1f s: quality %s, %s satellites, HDOP %s",
		    now(CLOCK_MONOTONIC) - started, field[6], field[7], field[8]);
	}
}

static void forward(const char *data, size_t len)
{
	/* NMEA sentences are embedded as printable "$GxXXX,...*HH" runs. */
	for (const char *p = memchr(data, '$', len); p; p = memchr(p + 1, '$', data + len - p - 1)) {
		const char *end = p + 1;

		while (end < data + len && *end >= ' ' && *end <= '~')
			end++;
		/* Standard sentences only; "$P..." are MediaTek debug output. */
		if (end - p < 10 || end[-3] != '*' || p[1] == 'P')
			continue;

		char line[256];
		size_t n = end - p;

		if (n + 2 > sizeof(line))
			continue;
		memcpy(line, p, n);
		memcpy(line + n, "\r\n", 2);
		track_fix(line, n);
		for (int i = nclients - 1; i >= 0; i--)
			if (send(clients[i], line, n + 2, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)(n + 2))
				drop_client(i);
		if (end >= data + len)
			break;
	}
}

static void on_report(void)
{
	unsigned char data[65536];
	ssize_t len = recv(reports, data, sizeof(data), 0);
	int32_t type;

	if (len < 8)
		return;
	memcpy(&type, data + 4, sizeof(type));
	if (type == MNL_REBOOT && started >= 0) {
		say("mnld restarted; restarting receiver");
		start_receiver();
	} else if (type == MNL_NMEA) {
		forward((const char *)data + 8, len - 8);
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr;
	int listener;

	if (argc != 2 || strlen(argv[1]) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "usage: %s SOCKET\n", argv[0]);
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);

	reports = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	requests = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (reports < 0 || requests < 0 ||
	    bind(reports, (struct sockaddr *)&addr, abstract_addr(&addr, "mtk_mnl2hal"))) {
		perror("mtk_mnl2hal");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, argv[1]);
	unlink(argv[1]);
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
	    chmod(argv[1], 0660) || listen(listener, MAX_CLIENTS)) {
		perror(argv[1]);
		return 1;
	}

	for (;;) {
		struct pollfd fds[2 + MAX_CLIENTS] = {
			{ .fd = reports, .events = POLLIN },
			{ .fd = listener, .events = nclients < MAX_CLIENTS ? POLLIN : 0 },
		};
		int timeout = -1;

		for (int i = 0; i < nclients; i++)
			fds[2 + i] = (struct pollfd){ .fd = clients[i], .events = POLLIN };
		if (stop_at >= 0)
			timeout = (stop_at - now(CLOCK_MONOTONIC)) * 1000 + 1;
		if (poll(fds, 2 + nclients, timeout < 0 && stop_at >= 0 ? 0 : timeout) < 0 && errno != EINTR) {
			perror("poll");
			return 1;
		}

		/* Clients only ever read; any input or hangup ends them. */
		for (int i = nclients - 1; i >= 0; i--) {
			char discard[64];

			if (fds[2 + i].revents & (POLLHUP | POLLERR) ||
			    (fds[2 + i].revents & POLLIN && recv(clients[i], discard, sizeof(discard), MSG_DONTWAIT) <= 0))
				drop_client(i);
		}
		if (fds[1].revents & POLLIN) {
			int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

			if (client >= 0) {
				clients[nclients++] = client;
				stop_at = -1;
				if (started < 0)
					start_receiver();
			}
		}
		if (fds[0].revents & POLLIN)
			on_report();
		if (stop_at >= 0 && now(CLOCK_MONOTONIC) >= stop_at) {
			stop_at = -1;
			if (!nclients && started >= 0)
				stop_receiver();
		}
	}
}
