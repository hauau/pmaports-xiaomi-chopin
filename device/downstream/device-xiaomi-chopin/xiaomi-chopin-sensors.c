/* SPDX-License-Identifier: MIT */
/*
 * Load the stock ambient-light calibration into the MediaTek sensor hub.
 *
 * The hub multiplies BU27030 lux by a calibration gain that is zero until
 * userspace supplies it, so the ambient light sensor otherwise reports 0 lux
 * forever. Android's HAL pushes nvcfg's als_cali.json at boot; this does the
 * same through the sensor/2.0 hf_manager ABI and verifies the stored result.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define HF_SENSOR_LIGHT 5
#define HF_SENSOR_CONFIG_CALI 4
#define HF_CUST_CMD_CALI 0
#define ALS_CALI_VALUES 4
#define READY_TIMEOUT_SECONDS 30

/* drivers/misc/mediatek/sensor/2.0/core/hf_sensor_io.h, packed and 4-aligned. */
struct hf_manager_cmd {
	uint8_t sensor_type;
	uint8_t action;
	uint8_t length;
	uint8_t padding;
	int32_t data[12];
} __attribute__((packed, aligned(4)));

struct custom_cmd {
	float f_data[24];
	uint32_t action;
	uint8_t command;
	uint8_t tx_len;
	uint8_t rx_len;
	uint8_t padding;
	int32_t data[15];
} __attribute__((packed, aligned(4)));

struct ioctl_packet {
	uint8_t sensor_type;
	uint8_t padding[3];
	union {
		uint8_t status;
		struct custom_cmd cust;
		uint8_t byte[164];
	};
} __attribute__((packed, aligned(4)));

#define HF_REQUEST_REGISTER_STATUS _IOWR('a', 1, struct ioctl_packet)
#define HF_REQUEST_CUST_DATA _IOWR('a', 7, struct ioctl_packet)
#define HF_REQUEST_READY_STATUS _IOWR('a', 8, struct ioctl_packet)

_Static_assert(sizeof(struct hf_manager_cmd) == 52, "hf_manager_cmd ABI");
_Static_assert(sizeof(struct custom_cmd) == 164, "custom_cmd ABI");
_Static_assert(sizeof(struct ioctl_packet) == 168, "ioctl_packet ABI");

/* Parse {"als_cali": [a, b, c, d]}; anything else fails closed. */
static int parse_als_cali(const char *json, int32_t values[ALS_CALI_VALUES])
{
	const char *cursor = strstr(json, "\"als_cali\"");
	int i;

	if (!cursor)
		return -1;
	cursor = strchr(cursor, '[');
	if (!cursor)
		return -1;
	cursor++;
	for (i = 0; i < ALS_CALI_VALUES; i++) {
		char *end;
		long value;

		errno = 0;
		value = strtol(cursor, &end, 10);
		if (end == cursor || errno || value < INT32_MIN || value > INT32_MAX)
			return -1;
		values[i] = (int32_t)value;
		cursor = end + strspn(end, " \t\r\n");
		if (*cursor != (i == ALS_CALI_VALUES - 1 ? ']' : ','))
			return -1;
		cursor++;
	}
	return 0;
}

static int read_file(const char *path, char *buffer, size_t size)
{
	FILE *file = fopen(path, "re");
	size_t length;

	if (!file)
		return -1;
	length = fread(buffer, 1, size - 1, file);
	fclose(file);
	if (length == 0)
		return -1;
	buffer[length] = '\0';
	return 0;
}

static int hub_status(int fd, unsigned long request, uint8_t sensor_type)
{
	struct ioctl_packet packet = { .sensor_type = sensor_type };

	if (ioctl(fd, request, &packet) < 0)
		return -1;
	return packet.status ? 1 : 0;
}

static int wait_for_hub(int fd)
{
	const struct timespec interval = { 0, 250000000 };
	int tries;

	for (tries = 0; tries < READY_TIMEOUT_SECONDS * 4; tries++) {
		int ready = hub_status(fd, HF_REQUEST_READY_STATUS, HF_SENSOR_LIGHT);

		if (ready < 0)
			return -1;
		if (ready)
			return hub_status(fd, HF_REQUEST_REGISTER_STATUS, HF_SENSOR_LIGHT) == 1 ? 0 : -1;
		nanosleep(&interval, NULL);
	}
	errno = ETIMEDOUT;
	return -1;
}

int main(int argc, char **argv)
{
	char json[4096];
	int32_t values[ALS_CALI_VALUES];
	struct hf_manager_cmd cmd = {
		.sensor_type = HF_SENSOR_LIGHT,
		.action = HF_SENSOR_CONFIG_CALI,
		.length = sizeof(values),
	};
	struct ioctl_packet packet = { .sensor_type = HF_SENSOR_LIGHT };
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s als_cali.json\n", argv[0]);
		return 2;
	}
	if (read_file(argv[1], json, sizeof(json)) < 0 || parse_als_cali(json, values) < 0) {
		fprintf(stderr, "%s: no usable als_cali record\n", argv[1]);
		return 1;
	}
	memcpy(cmd.data, values, sizeof(values));

	fd = open("/dev/hf_manager", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("/dev/hf_manager");
		return 1;
	}
	if (wait_for_hub(fd) < 0) {
		perror("sensor hub not ready or no light sensor");
		return 1;
	}
	/* The hub callback status is returned instead of a byte count. */
	if (write(fd, &cmd, sizeof(cmd)) != 0) {
		perror("light calibration config");
		return 1;
	}
	packet.cust.action = HF_CUST_CMD_CALI;
	if (ioctl(fd, HF_REQUEST_CUST_DATA, &packet) < 0 ||
	    memcmp(packet.cust.data, values, sizeof(values)) != 0) {
		fprintf(stderr, "light calibration readback mismatch\n");
		return 1;
	}
	close(fd);
	printf("light calibration loaded: %d %d %d %d\n",
	       values[0], values[1], values[2], values[3]);
	return 0;
}
