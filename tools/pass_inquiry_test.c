/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Sends one INQUIRY through freebsd_send_scsi_cmd. QEMU answers this
 * command, so the CCB path can be checked without UFS hardware. The UFS
 * specific commands ufs-utils sends need a real device.
 */

#include <sys/types.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freebsd_transport.h"
#include "scsi_bsg_util.h"

#define INQUIRY_CMD	0x12
#define INQUIRY_CMDLEN	6
#define INQUIRY_LEN	36

/* scsi_bsg_util.o needs print_error; ufs.c has it but also has main. */
void
print_error(const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	vfprintf(stderr, msg, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

int
main(int argc, char **argv)
{
	uint8_t cdb[INQUIRY_CMDLEN] = { INQUIRY_CMD, 0, 0, 0, INQUIRY_LEN, 0 };
	uint8_t buf[INQUIRY_LEN] = { 0 };
	const char *path = argc > 1 ? argv[1] : "/dev/pass2";
	int fd, ret, i;

	fd = open(path, O_RDWR);
	if (fd < 0)
		err(1, "open %s", path);

	ret = freebsd_send_scsi_cmd(fd, cdb, buf, INQUIRY_CMDLEN,
	    INQUIRY_LEN, SG_DXFER_FROM_DEV);
	if (ret < 0)
		errx(1, "freebsd_send_scsi_cmd returned %d", ret);

	printf("inquiry:");
	for (i = 0; i < 16; i++)
		printf(" %02x", buf[i]);
	printf("\nvendor: %.8s\nproduct: %.16s\n", buf + 8, buf + 16);

	/* Byte 0 is the peripheral device type. A disk is 0x00. */
	if (buf[0] != 0x00)
		errx(1, "device type is 0x%02x, expected 0x00", buf[0]);
	if (buf[8] == 0)
		errx(1, "vendor string is empty, nothing was read");

	close(fd);
	printf("PASS\n");
	return (0);
}
