/* SPDX-License-Identifier: MIT */
#include <err.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONNINFRA_INIT _IOR(0xc2, 2, int)
#define NVRAM_MAX 8192

int main(int argc, char **argv)
{
	static const char prefix[] = "WR-BUF:NVRAM";
	unsigned char command[sizeof(prefix) - 1 + NVRAM_MAX];
	struct stat st;
	size_t size;
	int fd;
	ssize_t count;

	if (argc != 2)
		errx(1, "usage: %s NVRAM-WIFI-record", argv[0]);

	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st) < 0)
		err(1, "%s", argv[1]);
	/* Stock wlan_assistant excludes the two-byte NVRAM trailer. */
	if (!S_ISREG(st.st_mode) || st.st_size <= 2 ||
	    st.st_size > NVRAM_MAX + 2 || (st.st_size - 2) % 256)
		errx(1, "%s: invalid NVRAM record size", argv[1]);
	size = (size_t)(st.st_size - 2);
	memcpy(command, prefix, sizeof(prefix) - 1);
	count = read(fd, command + sizeof(prefix) - 1, size);
	if (count < 0)
		err(1, "read %s", argv[1]);
	if ((size_t)count != size)
		errx(1, "%s: truncated NVRAM record", argv[1]);
	close(fd);

	fd = open("/dev/conninfra_dev", O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, CONNINFRA_INIT, 0) < 0)
		err(1, "initialize conninfra");
	close(fd);

	fd = open("/dev/wmtWifi", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		err(1, "open /dev/wmtWifi");
	size += sizeof(prefix) - 1;
	count = write(fd, command, size);
	if (count < 0)
		err(1, "load Wi-Fi NVRAM");
	if ((size_t)count != size)
		errx(1, "incomplete Wi-Fi NVRAM write");
	count = write(fd, "1", 1);
	if (count < 0)
		err(1, "power on Wi-Fi");
	if (count != 1)
		errx(1, "incomplete Wi-Fi power-on command");
	close(fd);
	return 0;
}
