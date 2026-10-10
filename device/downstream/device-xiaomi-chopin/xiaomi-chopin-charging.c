/* SPDX-License-Identifier: MIT */
#include <stdbool.h>

struct charging_config {
	int start, stop;
	int cool_current, warm_current;
	int warm_temp, stop_temp, resume_temp;
};

struct charging_state {
	bool window, warm, hot;
};

/* Temperatures are tenths of a degree Celsius; currents are microamps. */
static int charging_limit(struct charging_state *state,
	const struct charging_config *config, int capacity, int temperature, int online)
{
	if (capacity < 0 || capacity > 100 || temperature < -200 ||
	    temperature > 600 || (online != 0 && online != 1)) {
		*state = (struct charging_state){0};
		return 0;
	}

	if (capacity >= config->stop)
		state->window = false;
	else if (capacity <= config->start)
		state->window = true;

	if (temperature >= config->stop_temp)
		state->hot = true;
	else if (temperature <= config->resume_temp)
		state->hot = false;

	if (temperature >= config->warm_temp)
		state->warm = true;
	else if (temperature <= config->resume_temp)
		state->warm = false;

	/* Do not charge a cold battery, even if the vendor JEITA policy allows it. */
	if (!online || !state->window || state->hot || temperature < 100)
		return 0;
	return state->warm ? config->warm_current : config->cool_current;
}

#ifdef CHARGING_POLICY_TEST
#include <assert.h>
#include <stdio.h>

int main(void)
{
	const struct charging_config config = {55, 75, 500000, 250000, 320, 350, 300};
	struct charging_state state = {0};

	/* A restart in the middle of the window must not top up the battery. */
	assert(charging_limit(&state, &config, 60, 250, 1) == 0);
	assert(charging_limit(&state, &config, 55, 250, 1) == 500000);
	assert(charging_limit(&state, &config, 56, 250, 1) == 500000);
	assert(charging_limit(&state, &config, 74, 250, 1) == 500000);
	assert(charging_limit(&state, &config, 75, 250, 1) == 0);
	assert(charging_limit(&state, &config, 74, 250, 1) == 0);
	assert(charging_limit(&state, &config, 56, 250, 1) == 0);
	assert(charging_limit(&state, &config, 54, 250, 1) == 500000);

	/* Thermal hysteresis must not oscillate around either warm boundary. */
	assert(charging_limit(&state, &config, 60, 320, 1) == 250000);
	assert(charging_limit(&state, &config, 60, 310, 1) == 250000);
	assert(charging_limit(&state, &config, 60, 300, 1) == 500000);
	assert(charging_limit(&state, &config, 60, 350, 1) == 0);
	assert(charging_limit(&state, &config, 60, 340, 1) == 0);
	assert(charging_limit(&state, &config, 60, 310, 1) == 0);
	assert(charging_limit(&state, &config, 60, 300, 1) == 500000);

	/* A cable cycle retains the charge-window decision, but cannot charge offline. */
	assert(charging_limit(&state, &config, 60, 250, 0) == 0);
	assert(charging_limit(&state, &config, 60, 250, 1) == 500000);
	assert(charging_limit(&state, &config, 75, 250, 0) == 0);
	assert(charging_limit(&state, &config, 60, 250, 1) == 0);
	assert(charging_limit(&state, &config, 55, 99, 1) == 0);
	assert(charging_limit(&state, &config, 55, 100, 1) == 500000);

	/* Missing or nonsensical readings fail closed, including recovery in the band. */
	assert(charging_limit(&state, &config, -1, 250, 1) == 0);
	assert(charging_limit(&state, &config, 60, 250, 1) == 0);
	assert(charging_limit(&state, &config, 55, 250, 1) == 500000);
	assert(charging_limit(&state, &config, 101, 250, 1) == 0);
	assert(charging_limit(&state, &config, 55, -201, 1) == 0);
	assert(charging_limit(&state, &config, 55, 601, 1) == 0);
	assert(charging_limit(&state, &config, 55, 250, -1) == 0);
	assert(charging_limit(&state, &config, 100, 250, 1) == 0);
	puts("PASS: charge window, restart, thermal hysteresis, cable cycle, cold and sensor failures");
	return 0;
}
#else
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#define BATTERY "/sys/class/power_supply/battery/"
#define CURRENT_LIMIT BATTERY "constant_charge_current_max"
#define MASTER_HV "/sys/class/power_supply/bq2597x-master/hv_charge_enable"
#define SLAVE_HV "/sys/class/power_supply/bq2597x-slave/hv_charge_enable"

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number)
{
	(void)signal_number;
	running = 0;
}

static bool read_number(const char *path, int *value)
{
	char text[32], *end;
	long number;
	ssize_t count;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return false;
	count = read(fd, text, sizeof(text) - 1);
	close(fd);
	if (count <= 0 || count == (ssize_t)sizeof(text) - 1) {
		errno = EINVAL;
		return false;
	}
	text[count] = '\0';
	errno = 0;
	number = strtol(text, &end, 10);
	if (end == text || errno || number < INT_MIN || number > INT_MAX) {
		errno = EINVAL;
		return false;
	}
	while (isspace((unsigned char)*end))
		end++;
	if (*end) {
		errno = EINVAL;
		return false;
	}
	*value = (int)number;
	return true;
}

static bool write_number(const char *path, int value)
{
	char text[32];
	int length = snprintf(text, sizeof(text), "%d\n", value);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t count;
	int write_errno;

	if (fd < 0)
		return false;
	count = write(fd, text, (size_t)length);
	write_errno = errno;
	close(fd);
	if (count != length) {
		errno = count < 0 ? write_errno : EIO;
		return false;
	}
	return true;
}

static bool ensure_number(const char *path, int value)
{
	int actual;

	if (!read_number(path, &actual))
		return false;
	if (actual == value)
		return true;
	if (!write_number(path, value) || !read_number(path, &actual))
		return false;
	if (actual != value) {
		errno = EIO;
		return false;
	}
	return true;
}

static int argument(const char *text)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (end == text || *end || errno || value < 0 || value > INT_MAX)
		errx(1, "invalid policy argument: %s", text);
	return (int)value;
}

int main(int argc, char **argv)
{
	struct charging_config config;
	struct charging_state state = {0};
	int previous_limit = -1;
	bool sensor_error = false;
	struct sigaction action = {.sa_handler = stop_running};

	openlog("xiaomi-chopin-charging", LOG_PID, LOG_DAEMON);
	if (!ensure_number(CURRENT_LIMIT, 0) || !ensure_number(MASTER_HV, 0) ||
	    !ensure_number(SLAVE_HV, 0)) {
		write_number(CURRENT_LIMIT, 0);
		err(1, "initialize safe charging controls");
	}
	if (argc != 8)
		errx(1, "usage: %s START%% STOP%% COOL_uA WARM_uA WARM_dC STOP_dC RESUME_dC", argv[0]);
	config = (struct charging_config){argument(argv[1]), argument(argv[2]),
		argument(argv[3]), argument(argv[4]), argument(argv[5]),
		argument(argv[6]), argument(argv[7])};
	if (config.start >= config.stop || config.stop > 100 ||
	    config.cool_current < 100000 || config.cool_current > 1000000 ||
	    config.warm_current < 100000 || config.warm_current > config.cool_current ||
	    config.resume_temp < 100 || config.resume_temp >= config.warm_temp ||
	    config.warm_temp >= config.stop_temp || config.stop_temp > 400)
		errx(1, "invalid charge window, current limits or thermal thresholds");

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0)
		err(1, "install shutdown handler");

	while (running) {
		int capacity = -1, temperature = -1000, online = -1;
		int limit;
		bool valid = read_number(BATTERY "capacity", &capacity) &&
			read_number(BATTERY "temp", &temperature) &&
			read_number("/sys/class/power_supply/charger/online", &online);

		if (!valid) {
			capacity = -1;
			if (!sensor_error)
				syslog(LOG_ERR, "battery telemetry unavailable; charging paused");
		}
		sensor_error = !valid;
		limit = charging_limit(&state, &config, capacity, temperature, online);
		if (!ensure_number(MASTER_HV, 0) || !ensure_number(SLAVE_HV, 0) ||
		    !ensure_number(CURRENT_LIMIT, limit)) {
			write_number(CURRENT_LIMIT, 0);
			err(1, "apply charging policy");
		}
		if (limit != previous_limit) {
			syslog(LOG_INFO, "capacity=%d%% temperature=%d/10C current_limit=%duA window=%d hot=%d warm=%d",
				capacity, temperature, limit, state.window, state.hot, state.warm);
			previous_limit = limit;
		}
		sleep(10);
	}
	if (!write_number(CURRENT_LIMIT, 0))
		err(1, "pause charging on shutdown");
	return 0;
}
#endif
