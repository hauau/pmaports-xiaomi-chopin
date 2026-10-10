// SPDX-License-Identifier: MIT
/*
 * Helpers for running untouched stock Android vendor daemons of the Xiaomi
 * chopin from its read-only stock partitions:
 *
 *   super-extents SUPER NAME...   print "NAME START SECTORS" (512-byte
 *                                 sectors) of single-extent logical
 *                                 partitions in SUPER's liblp metadata
 *   properties FILE [KEY=VALUE]... write a read-only bionic property area
 *   dnsproxyd SOCKET              answer bionic getaddrinfo() requests that
 *                                 Android sends to netd's dnsproxyd
 *   exec ROOT USER GIDS CAPS CMD [ARG]...
 *                                 chroot, drop to USER plus the extra numeric
 *                                 GIDS, keep only the named CAPS (e.g.
 *                                 wake_alarm) as ambient capabilities, exec
 *                                 CMD; "-" for no GIDS or CAPS
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int fail(const char *what)
{
	fprintf(stderr, "xiaomi-chopin-android: %s: %s\n", what, strerror(errno));
	return 1;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* liblp: geometry at 4 KiB, slot 0 primary metadata after two geometry copies. */
#define LP_GEOMETRY_OFFSET 4096
#define LP_METADATA_OFFSET (4096 + 2 * 4096)
#define LP_GEOMETRY_MAGIC 0x616c4467
#define LP_HEADER_MAGIC 0x414c5030
#define LP_PARTITION_SIZE 52
#define LP_EXTENT_SIZE 24

static int super_extents(int argc, char **argv)
{
	uint8_t geometry[52], header[128];
	int fd = open(argv[0], O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return fail(argv[0]);
	if (pread(fd, geometry, sizeof(geometry), LP_GEOMETRY_OFFSET) != (ssize_t)sizeof(geometry) ||
	    pread(fd, header, sizeof(header), LP_METADATA_OFFSET) != (ssize_t)sizeof(header))
		return fail("reading metadata");
	if (le32(geometry) != LP_GEOMETRY_MAGIC || le32(header) != LP_HEADER_MAGIC) {
		fprintf(stderr, "xiaomi-chopin-android: no liblp metadata on %s\n", argv[0]);
		return 1;
	}

	uint32_t max_size = le32(geometry + 40), header_size = le32(header + 8);
	uint32_t tables_size = le32(header + 44);
	uint32_t part_off = le32(header + 80), part_num = le32(header + 84), part_size = le32(header + 88);
	uint32_t ext_off = le32(header + 92), ext_num = le32(header + 96), ext_size = le32(header + 100);

	/* v10.2 headers are 256 bytes; every field used here is in the first 128. */
	if (header_size < sizeof(header) || header_size > 4096 || tables_size > max_size ||
	    part_size < LP_PARTITION_SIZE || ext_size < LP_EXTENT_SIZE ||
	    (uint64_t)part_off + (uint64_t)part_num * part_size > tables_size ||
	    (uint64_t)ext_off + (uint64_t)ext_num * ext_size > tables_size) {
		fprintf(stderr, "xiaomi-chopin-android: inconsistent liblp header\n");
		return 1;
	}

	uint8_t *tables = malloc(tables_size);

	if (!tables || pread(fd, tables, tables_size, LP_METADATA_OFFSET + header_size) != (ssize_t)tables_size)
		return fail("reading metadata tables");
	close(fd);

	int status = 0;

	for (int i = 1; i < argc; i++) {
		int found = 0;

		for (uint32_t p = 0; p < part_num; p++) {
			const uint8_t *part = tables + part_off + p * part_size;

			if (strnlen((const char *)part, 36) == 36 || strcmp((const char *)part, argv[i]))
				continue;
			found = 1;

			uint32_t first = le32(part + 40), count = le32(part + 44);
			const uint8_t *ext = tables + ext_off + (uint64_t)first * ext_size;

			/* Only a single linear extent on super itself maps 1:1. */
			if (count != 1 || first >= ext_num || le32(ext + 8) != 0 || le32(ext + 20) != 0) {
				fprintf(stderr, "xiaomi-chopin-android: %s is not one linear extent\n", argv[i]);
				status = 1;
				break;
			}
			printf("%s %llu %llu\n", argv[i], (unsigned long long)le64(ext + 12),
			       (unsigned long long)le64(ext));
			break;
		}
		if (!found) {
			fprintf(stderr, "xiaomi-chopin-android: no logical partition %s\n", argv[i]);
			status = 1;
		}
	}
	free(tables);
	return status;
}

/*
 * bionic system_properties prop_area: 128-byte header, then a trie of
 * prop_bt nodes (namelen, prop, left, right, children, name) and prop_info
 * records (serial with the value length in the top byte, value[92], name).
 */
#define PROP_AREA_SIZE (128 * 1024)
#define PROP_HEADER 128
#define PROP_VALUE_MAX 92

struct area {
	uint8_t data[PROP_AREA_SIZE - PROP_HEADER];
	uint32_t used;
};

static uint32_t area_alloc(struct area *a, size_t size)
{
	uint32_t off = a->used;

	size = (size + 3) & ~(size_t)3;
	if (size > sizeof(a->data) - a->used) {
		fprintf(stderr, "xiaomi-chopin-android: property area full\n");
		exit(1);
	}
	a->used += size;
	return off;
}

static uint32_t *bt_field(struct area *a, uint32_t bt, int index)
{
	return (uint32_t *)(a->data + bt + 4 * (1 + index)); /* prop, left, right, children */
}

static uint32_t new_bt(struct area *a, const char *name, size_t len)
{
	uint32_t bt = area_alloc(a, 20 + len + 1);

	*(uint32_t *)(a->data + bt) = len;
	memcpy(a->data + bt + 20, name, len);
	return bt;
}

/* bionic cmp_prop_name: shorter names sort first, then bytewise. */
static int cmp_name(const char *a, size_t alen, const char *b, size_t blen)
{
	if (alen != blen)
		return alen < blen ? -1 : 1;
	return memcmp(a, b, alen);
}

static uint32_t find_bt(struct area *a, uint32_t bt, const char *name, size_t len)
{
	for (;;) {
		int ret = cmp_name(name, len, (const char *)a->data + bt + 20, *(uint32_t *)(a->data + bt));

		if (!ret)
			return bt;

		uint32_t *next = bt_field(a, bt, ret < 0 ? 1 : 2);

		if (!*next) {
			uint32_t created = new_bt(a, name, len);

			*bt_field(a, bt, ret < 0 ? 1 : 2) = created;
			return created;
		}
		bt = *next;
	}
}

static void area_add(struct area *a, const char *key, const char *value)
{
	size_t key_len = strlen(key), value_len = strlen(value);
	uint32_t cur = 0;

	if (value_len >= PROP_VALUE_MAX) {
		fprintf(stderr, "xiaomi-chopin-android: value of %s too long\n", key);
		exit(1);
	}
	for (const char *part = key;;) {
		const char *dot = strchr(part, '.');
		size_t len = dot ? (size_t)(dot - part) : strlen(part);
		uint32_t child = *bt_field(a, cur, 3);

		if (!child) {
			child = new_bt(a, part, len);
			*bt_field(a, cur, 3) = child;
		} else {
			child = find_bt(a, child, part, len);
		}
		cur = child;
		if (!dot)
			break;
		part = dot + 1;
	}

	uint32_t info = area_alloc(a, 4 + PROP_VALUE_MAX + key_len + 1);

	*(uint32_t *)(a->data + info) = (uint32_t)value_len << 24;
	memcpy(a->data + info + 4, value, value_len);
	memcpy(a->data + info + 4 + PROP_VALUE_MAX, key, key_len);
	*bt_field(a, cur, 0) = info;
}

/* /dev/conninfra_dev: _IOR(0xc2, 0) chip id, _IOR(0xc2, 3) A-die id. */
static int conninfra_id(int fd, int nr)
{
	return ioctl(fd, (2U << 30) | (4U << 16) | (0xc2U << 8) | nr, 0);
}

static int properties(int argc, char **argv)
{
	static struct area area;
	char chip[16], adie[16], tmp[PATH_MAX];
	int fd = open("/dev/conninfra_dev", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return fail("/dev/conninfra_dev");
	snprintf(chip, sizeof(chip), "0x%04x", conninfra_id(fd, 0));
	snprintf(adie, sizeof(adie), "0x%04x", conninfra_id(fd, 3));
	close(fd);

	area.used = 0;
	new_bt(&area, "", 0); /* root */

	const char *defaults[][2] = {
		{ "ro.build.type", "user" },
		{ "ro.debuggable", "0" },
		{ "ro.vndk.version", "31" },
		{ "ro.board.platform", "mt6893" },
		{ "ro.hardware", "mt6893" },
		{ "ro.product.vendor.device", "chopin" },
		{ "ro.property_service.version", "2" },
		{ "persist.vendor.connsys.chipid", chip },
		{ "vendor.connsys.adie.chipid", adie },
		{ "vendor.connsys.driver.ready", "yes" },
		{ "vendor.service.nvram_init", "Ready" },
	};
	for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		int overridden = 0;

		for (int j = 1; j < argc; j++)
			overridden |= !strncmp(argv[j], defaults[i][0], strlen(defaults[i][0])) &&
				      argv[j][strlen(defaults[i][0])] == '=';
		if (!overridden)
			area_add(&area, defaults[i][0], defaults[i][1]);
	}
	for (int j = 1; j < argc; j++) {
		char *eq = strchr(argv[j], '=');

		if (!eq) {
			fprintf(stderr, "xiaomi-chopin-android: expected KEY=VALUE: %s\n", argv[j]);
			return 1;
		}
		*eq = '\0';
		area_add(&area, argv[j], eq + 1);
	}

	uint32_t header[PROP_HEADER / 4] = { area.used, 0, 0x504f5250, 0xfc6ed0ab };

	snprintf(tmp, sizeof(tmp), "%s.tmp", argv[0]);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
	if (fd < 0)
		return fail(tmp);
	if (write(fd, header, sizeof(header)) != (ssize_t)sizeof(header) ||
	    write(fd, area.data, sizeof(area.data)) != (ssize_t)sizeof(area.data) || close(fd))
		return fail(tmp);
	if (rename(tmp, argv[0]))
		return fail(argv[0]);
	return 0;
}

/* bionic <netdb.h> numbering differs from musl's. */
static const struct { int musl, bionic; } eai_map[] = {
	{ EAI_AGAIN, 2 }, { EAI_BADFLAGS, 3 }, { EAI_FAIL, 4 }, { EAI_FAMILY, 5 },
	{ EAI_MEMORY, 6 }, { EAI_NONAME, 8 }, { EAI_SERVICE, 9 }, { EAI_SOCKTYPE, 10 },
	{ EAI_SYSTEM, 11 },
};
static const struct { int bionic, musl; } ai_flag_map[] = {
	{ 0x1, AI_PASSIVE }, { 0x2, AI_CANONNAME }, { 0x4, AI_NUMERICHOST },
	{ 0x8, AI_NUMERICSERV }, { 0x100, AI_ALL }, { 0x400, AI_ADDRCONFIG },
	{ 0x800, AI_V4MAPPED },
};

static int write_all(int fd, const void *buf, size_t len)
{
	for (const char *p = buf; len;) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static int put_be32(char **p, char *end, uint32_t value)
{
	if (end - *p < 4)
		return -1;
	value = htobe32(value);
	memcpy(*p, &value, 4);
	*p += 4;
	return 0;
}

static int put_bytes(char **p, char *end, const void *data, uint32_t len)
{
	if (put_be32(p, end, len) || end - *p < (ptrdiff_t)len)
		return -1;
	memcpy(*p, data, len);
	*p += len;
	return 0;
}

/* Answer one "getaddrinfo HOST SERV FLAGS FAMILY SOCKTYPE PROTOCOL NETID". */
static int dns_query(int fd, char *command)
{
	char *word[8], *save = NULL;
	int n = 0;

	for (char *w = strtok_r(command, " ", &save); w && n < 8; w = strtok_r(NULL, " ", &save))
		word[n++] = w;

	int code = 4; /* EAI_FAIL */

	if (n == 8 && !strcmp(word[0], "getaddrinfo")) {
		int bionic_flags = atoi(word[3]);
		struct addrinfo hints = {
			.ai_family = atoi(word[4]) > 0 ? atoi(word[4]) : AF_UNSPEC,
			.ai_socktype = atoi(word[5]) > 0 ? atoi(word[5]) : 0,
			.ai_protocol = atoi(word[6]) > 0 ? atoi(word[6]) : 0,
		}, *res;

		for (size_t i = 0; bionic_flags > 0 && i < sizeof(ai_flag_map) / sizeof(ai_flag_map[0]); i++)
			if (bionic_flags & ai_flag_map[i].bionic)
				hints.ai_flags |= ai_flag_map[i].musl;

		int ret = getaddrinfo(strcmp(word[1], "^") ? word[1] : NULL,
				      strcmp(word[2], "^") ? word[2] : NULL, &hints, &res);

		if (!ret) {
			static char reply[16384];
			char *p = reply, *end = reply + sizeof(reply);
			int ok = 1;

			memcpy(p, "222", 4); /* DnsProxyQueryResult */
			p += 4;
			for (struct addrinfo *ai = res; ai && ok; ai = ai->ai_next) {
				if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
					continue;
				ok = !put_be32(&p, end, 1) && !put_be32(&p, end, bionic_flags > 0 ? bionic_flags : 0) &&
				     !put_be32(&p, end, ai->ai_family) && !put_be32(&p, end, ai->ai_socktype) &&
				     !put_be32(&p, end, ai->ai_protocol) &&
				     !put_bytes(&p, end, ai->ai_addr, ai->ai_addrlen) &&
				     !put_bytes(&p, end, ai->ai_canonname,
						ai->ai_canonname ? strlen(ai->ai_canonname) + 1 : 0);
			}
			freeaddrinfo(res);
			if (ok && !put_be32(&p, end, 0))
				return write_all(fd, reply, p - reply);
			code = 6; /* EAI_MEMORY: reply too large */
		} else {
			for (size_t i = 0; i < sizeof(eai_map) / sizeof(eai_map[0]); i++)
				if (eai_map[i].musl == ret)
					code = eai_map[i].bionic;
		}
	}

	/* DnsProxyOperationFailed: code, big-endian length, then the raw int
	 * as netd's SocketClient::sendBinaryMsg() sends it. */
	char reply[12] = "401";
	char *p = reply + 4;

	put_be32(&p, reply + sizeof(reply), sizeof(code));
	memcpy(p, &code, sizeof(code));
	return write_all(fd, reply, sizeof(reply));
}

static int dnsproxyd(char *path)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (strlen(path) >= sizeof(addr.sun_path))
		return fail("socket path");
	strcpy(addr.sun_path, path);
	unlink(path);
	if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
	    chmod(path, 0666) || listen(listener, 16))
		return fail(path);
	signal(SIGPIPE, SIG_IGN);

	for (;;) {
		int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

		if (client < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			return fail("accept");
		}

		/* Bionic opens one connection per query; bound a stalled peer. */
		struct timeval timeout = { .tv_sec = 5 };
		char command[1024];
		size_t len = 0;

		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		for (;;) {
			ssize_t n = read(client, command + len, 1);

			if (n <= 0)
				break;
			if (command[len] == '\0') {
				if (dns_query(client, command))
					break;
				len = 0;
			} else if (++len == sizeof(command)) {
				break;
			}
		}
		close(client);
	}
}

static const struct { const char *name; int cap; } cap_names[] = {
	{ "wake_alarm", CAP_WAKE_ALARM },
	{ "block_suspend", CAP_BLOCK_SUSPEND },
	{ "sys_nice", CAP_SYS_NICE },
};

static int exec_as(char **argv)
{
	struct passwd *pw = getpwnam(argv[1]);
	struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct data[2] = { { 0 } };
	uint64_t caps = 0;
	gid_t gids[32];
	int ngids = 0;

	if (!pw) {
		fprintf(stderr, "xiaomi-chopin-android: no user %s\n", argv[1]);
		return 1;
	}
	gids[ngids++] = pw->pw_gid;
	if (strcmp(argv[2], "-"))
		for (char *gid = strtok(argv[2], ","); gid && ngids < 32; gid = strtok(NULL, ","))
			gids[ngids++] = strtoul(gid, NULL, 10);
	if (strcmp(argv[3], "-")) {
		for (char *name = strtok(argv[3], ","); name; name = strtok(NULL, ",")) {
			size_t i = 0;

			while (i < sizeof(cap_names) / sizeof(cap_names[0]) && strcmp(cap_names[i].name, name))
				i++;
			if (i == sizeof(cap_names) / sizeof(cap_names[0])) {
				fprintf(stderr, "xiaomi-chopin-android: unknown capability %s\n", name);
				return 1;
			}
			caps |= 1ULL << cap_names[i].cap;
		}
	}

	if (chroot(argv[0]) || chdir("/"))
		return fail(argv[0]);
	/* Keep the permitted set across setuid only to narrow it below. */
	if (caps && prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0))
		return fail("PR_SET_KEEPCAPS");
	if (setgroups(ngids, gids) || setgid(pw->pw_gid) || setuid(pw->pw_uid))
		return fail("dropping privileges");
	if (caps) {
		for (int i = 0; i < 2; i++)
			data[i].permitted = data[i].effective = data[i].inheritable = caps >> (32 * i);
		if (syscall(SYS_capset, &header, data))
			return fail("capset");
		for (int cap = 0; cap < 64; cap++)
			if (caps & 1ULL << cap && prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0))
				return fail("PR_CAP_AMBIENT_RAISE");
	}
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return fail("PR_SET_NO_NEW_PRIVS");
	execv(argv[4], argv + 4);
	return fail(argv[4]);
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "super-extents"))
		return super_extents(argc - 2, argv + 2);
	if (argc >= 3 && !strcmp(argv[1], "properties"))
		return properties(argc - 2, argv + 2);
	if (argc == 3 && !strcmp(argv[1], "dnsproxyd"))
		return dnsproxyd(argv[2]);
	if (argc >= 7 && !strcmp(argv[1], "exec"))
		return exec_as(argv + 2);
	fprintf(stderr, "usage: %s super-extents SUPER NAME... | properties FILE [KEY=VALUE]... |\n"
			"       dnsproxyd SOCKET | exec ROOT USER GIDS|- CAPS|- CMD [ARG]...\n", argv[0]);
	return 2;
}
