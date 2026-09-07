/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Throw malformed passthrough requests at the driver and check it keeps
 * its footing. Nothing here should ever reach the device intact: the
 * point is that the kernel refuses or fails cleanly instead of faulting.
 *
 * The seed is printed so a crash can be repeated.
 */

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/ufshci/ufshci.h>
#include <dev/ufshci/ufshci_ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t rng_state;

static uint32_t
rnd(void)
{
	/* Small LCG. Repeatable from the seed, which is all this needs. */
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (uint32_t)(rng_state >> 33);
}

static void
fill_random(void *p, size_t n)
{
	uint8_t *b = p;
	size_t i;

	for (i = 0; i < n; i++)
		b[i] = rnd() & 0xff;
}

/* Transaction codes the driver says it carries, plus ones it should not. */
static const uint8_t codes[] = { 0x00, 0x01, 0x16, 0x02, 0x04, 0x21, 0x36,
	0x3f, 0xff };

/*
 * Safe mode is for a device someone cares about. A random command UPIU
 * carries a random CDB, which can be a write or a firmware download, and a
 * random query can be a write to a persistent attribute. Neither is
 * something to aim at a real disk. In safe mode the steered half is a read
 * only query, and the rest stays wholly random, which the driver refuses
 * long before it reaches the device. This trades away the command UPIU
 * path, which the run against an emulated device covers instead.
 */
static bool safe_mode;

static const uint8_t read_opcodes[] = {
	UFSHCI_QUERY_OPCODE_READ_DESCRIPTOR,
	UFSHCI_QUERY_OPCODE_READ_ATTRIBUTE,
	UFSHCI_QUERY_OPCODE_READ_FLAG,
};

static void
steer_safe(struct ufshci_pt_command *pt)
{
	struct ufshci_query_request_upiu *q =
	    (struct ufshci_query_request_upiu *)&pt->req_upiu;

	q->header.trans_type = UFSHCI_UPIU_TRANSACTION_CODE_QUERY_REQUEST;
	q->header.ehs_length = 0;
	q->header.ext_iid_or_function =
	    UFSHCI_QUERY_FUNC_STANDARD_READ_REQUEST;
	q->opcode = read_opcodes[rnd() % nitems(read_opcodes)];

	pt->buf = NULL;
	pt->len = 0;
	pt->flags = 0;
}

static void
fuzz_command(int fd, unsigned rounds)
{
	struct ufshci_pt_command pt;
	uint8_t buf[512];
	unsigned i;

	for (i = 0; i < rounds; i++) {
		fill_random(&pt, sizeof(pt));
		fill_random(buf, sizeof(buf));

		/*
		 * The unsteered half is refused on its random timeout_ms
		 * before anything else is read. Clearing the write bit of
		 * the query function costs nothing and does not lean on
		 * that.
		 */
		if (safe_mode)
			pt.req_upiu.header.ext_iid_or_function &= ~0x80;

		/*
		 * A wholly random struct is almost always rejected on the
		 * first check, so steer some of it toward the deeper paths.
		 */
		if (i % 2 == 0) {
			pt.timeout_ms = 0;
			pt.req_upiu.header.trans_type =
			    codes[rnd() % nitems(codes)];
			pt.flags = rnd() % 4;
			pt.len = (rnd() % 3 == 0) ? 0 : rnd() % 1024;
			pt.buf = (pt.len != 0) ? buf : NULL;
			if (pt.len > sizeof(buf))
				pt.len = sizeof(buf);
			if (safe_mode)
				steer_safe(&pt);
		}

		(void)ioctl(fd, UFSHCI_PASSTHROUGH_CMD, &pt);
	}
}

static void
fuzz_uic(int fd, unsigned rounds)
{
	struct ufshci_pt_uic_command uic;
	unsigned i;

	for (i = 0; i < rounds; i++) {
		fill_random(&uic, sizeof(uic));
		if (i % 2 == 0) {
			uic.timeout_ms = 0;
			/* Walk the whole opcode space, allowed or not. */
			uic.cmd.opcode = rnd() & 0x1f;
		}
		(void)ioctl(fd, UFSHCI_PASSTHROUGH_UIC, &uic);
	}
}

int
main(int argc, char **argv)
{
	const char *path = "/dev/ufshci0";
	unsigned rounds = 2000;
	int fd;

	while (argc > 1 && strcmp(argv[1], "-s") == 0) {
		safe_mode = true;
		argc--;
		argv++;
	}

	if (argc > 1)
		rounds = (unsigned)strtoul(argv[1], NULL, 0);
	if (argc > 2)
		path = argv[2];

	rng_state = (argc > 3) ? strtoull(argv[3], NULL, 0) : 20260822;
	printf("seed=%llu rounds=%u%s\n", (unsigned long long)rng_state, rounds,
	    safe_mode ? " safe" : "");

	fd = open(path, O_RDWR);
	if (fd < 0)
		err(1, "open %s", path);

	fuzz_command(fd, rounds);
	fuzz_uic(fd, rounds);

	close(fd);
	printf("SURVIVED\n");
	return (0);
}
