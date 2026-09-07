/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Smallest possible exercise of UFSHCI_PASSTHROUGH_CMD. Reads the device
 * descriptor. Kept apart from ufs-utils so a failure here points at the
 * kernel and nothing else.
 */

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/param.h>

#include <dev/ufshci/ufshci_ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DESC_MAX_SIZE 255

/*
 * Each of these must be refused, and refused for the stated reason. A
 * check that accepts any failure would pass when the driver lets bad
 * input through and the device rejects it instead.
 */
struct neg_case {
	const char *what;
	int want_errno;
	void (*setup)(struct ufshci_pt_command *, uint8_t *);
};

static void
neg_query(struct ufshci_pt_command *pt)
{
	memset(pt, 0, sizeof(*pt));
	pt->req_upiu.header.trans_type =
	    UFSHCI_UPIU_TRANSACTION_CODE_QUERY_REQUEST;
	pt->req_upiu.header.ext_iid_or_function =
	    UFSHCI_QUERY_FUNC_STANDARD_READ_REQUEST;
}

static void
set_big_len(struct ufshci_pt_command *pt, uint8_t *buf)
{
	neg_query(pt);
	pt->buf = buf;
	pt->len = UFSHCI_PT_MAX_XFER + 1;
	pt->flags = UFSHCI_PT_FLAG_DATA_IN;
}

static void
set_null_buf(struct ufshci_pt_command *pt, uint8_t *buf)
{
	(void)buf;
	neg_query(pt);
	pt->buf = NULL;
	pt->len = 64;
	pt->flags = UFSHCI_PT_FLAG_DATA_IN;
}

static void
set_bad_code(struct ufshci_pt_command *pt, uint8_t *buf)
{
	(void)buf;
	memset(pt, 0, sizeof(*pt));
	pt->req_upiu.header.trans_type = 0x3f;
}

static void
set_query_ehs(struct ufshci_pt_command *pt, uint8_t *buf)
{
	(void)buf;
	neg_query(pt);
	pt->req_upiu.header.ehs_length = 1;
}

static void
set_huge_ehs(struct ufshci_pt_command *pt, uint8_t *buf)
{
	(void)buf;
	memset(pt, 0, sizeof(*pt));
	pt->req_upiu.header.trans_type = UFSHCI_UPIU_TRANSACTION_CODE_COMMAND;
	pt->req_upiu.header.ehs_length = 0xff;
}

static void
set_both_dirs(struct ufshci_pt_command *pt, uint8_t *buf)
{
	neg_query(pt);
	pt->buf = buf;
	pt->len = 8;
	pt->flags = UFSHCI_PT_FLAG_DATA_IN | UFSHCI_PT_FLAG_DATA_OUT;
}

static void
set_timeout(struct ufshci_pt_command *pt, uint8_t *buf)
{
	(void)buf;
	neg_query(pt);
	pt->timeout_ms = 1000;
}

static void
set_bad_ptr(struct ufshci_pt_command *pt, uint8_t *buf)
{
	(void)buf;
	neg_query(pt);
	pt->buf = (void *)0x1;
	pt->len = 64;
	pt->flags = UFSHCI_PT_FLAG_DATA_OUT;
}

static void
set_no_dir(struct ufshci_pt_command *pt, uint8_t *buf)
{
	neg_query(pt);
	pt->buf = buf;
	pt->len = 8;
	pt->flags = 0;
}

static const struct neg_case neg_cases[] = {
	{ "data length past the cap",	   EINVAL, set_big_len },
	{ "NULL buffer with a length",	   EINVAL, set_null_buf },
	{ "unknown transaction code",	   EINVAL, set_bad_code },
	{ "an EHS on a query",		   EINVAL, set_query_ehs },
	{ "an EHS longer than the UPIU",   EINVAL, set_huge_ehs },
	{ "both data directions at once",  EINVAL, set_both_dirs },
	{ "a buffer with no direction",	   EINVAL, set_no_dir },
	{ "a timeout the driver ignores",  EINVAL, set_timeout },
	{ "a buffer the process does not own", EFAULT, set_bad_ptr },
};

static int
negative_cases(int fd)
{
	struct ufshci_pt_command pt;
	uint8_t small[8] = { 0 };
	int failures = 0;
	size_t i;

	for (i = 0; i < nitems(neg_cases); i++) {
		const struct neg_case *c = &neg_cases[i];

		c->setup(&pt, small);
		errno = 0;
		if (ioctl(fd, UFSHCI_PASSTHROUGH_CMD, &pt) == 0) {
			warnx("%s was accepted", c->what);
			failures++;
		} else if (errno != c->want_errno) {
			warnx("%s gave errno %d (%s), expected %d", c->what,
			    errno, strerror(errno), c->want_errno);
			failures++;
		}
	}

	return (failures);
}

/*
 * The UIC passthrough carries the four attribute commands only. The rest
 * can drop the link or power the device off.
 */
static int
uic_negative_cases(int fd)
{
	struct ufshci_pt_uic_command uic;
	int failures = 0;
	size_t i;
	static const struct {
		const char *what;
		uint8_t opcode;
	} banned[] = {
		{ "DME_RESET",		UFSHCI_DME_RESET },
		{ "DME_POWER_OFF",	UFSHCI_DME_POWER_OFF },
		{ "DME_LINK_STARTUP",	UFSHCI_DME_LINK_STARTUP },
		{ "DME_HIBERNATE_ENTER", UFSHCI_DME_HIBERNATE_ENTER },
	};

	for (i = 0; i < nitems(banned); i++) {
		memset(&uic, 0, sizeof(uic));
		uic.cmd.opcode = banned[i].opcode;
		errno = 0;
		if (ioctl(fd, UFSHCI_PASSTHROUGH_UIC, &uic) == 0) {
			warnx("%s was accepted", banned[i].what);
			failures++;
		} else if (errno != EINVAL) {
			warnx("%s gave errno %d (%s), expected %d",
			    banned[i].what, errno, strerror(errno), EINVAL);
			failures++;
		}
	}

	/* A timeout the driver cannot honor. */
	memset(&uic, 0, sizeof(uic));
	uic.cmd.opcode = UFSHCI_DME_GET;
	uic.timeout_ms = 1000;
	errno = 0;
	if (ioctl(fd, UFSHCI_PASSTHROUGH_UIC, &uic) == 0) {
		warnx("a non zero UIC timeout was accepted");
		failures++;
	} else if (errno != EINVAL) {
		warnx("UIC timeout gave errno %d (%s), expected %d", errno,
		    strerror(errno), EINVAL);
		failures++;
	}

	return (failures);
}

int
main(int argc, char **argv)
{
	struct ufshci_pt_command pt;
	struct ufshci_query_request_upiu *req;
	struct ufshci_query_response_upiu *resp;
	const uint8_t *desc;
	const char *path = argc > 1 ? argv[1] : "/dev/ufshci0";
	int fd, i;

	fd = open(path, O_RDWR);
	if (fd < 0)
		err(1, "open %s", path);

	memset(&pt, 0, sizeof(pt));

	req = (struct ufshci_query_request_upiu *)&pt.req_upiu;
	req->header.trans_type = UFSHCI_UPIU_TRANSACTION_CODE_QUERY_REQUEST;
	req->header.ext_iid_or_function =
	    UFSHCI_QUERY_FUNC_STANDARD_READ_REQUEST;
	req->opcode = UFSHCI_QUERY_OPCODE_READ_DESCRIPTOR;
	req->idn = UFSHCI_DESC_TYPE_DEVICE;
	req->index = 0;
	req->selector = 0;
	req->length = htobe16(DESC_MAX_SIZE);

	/*
	 * A query request carries its data inside the UPIU, so buf stays
	 * NULL and len stays zero. The descriptor comes back in the
	 * response UPIU's command_data.
	 */

	if (ioctl(fd, UFSHCI_PASSTHROUGH_CMD, &pt) < 0)
		err(1, "UFSHCI_PASSTHROUGH_CMD");

	resp = (struct ufshci_query_response_upiu *)&pt.resp_upiu;
	desc = resp->command_data;

	printf("ocs=0x%02x resp_len=%u\n", pt.ocs, be16toh(resp->length));
	printf("desc:");
	for (i = 0; i < 16; i++)
		printf(" %02x", desc[i]);
	printf("\n");

	if (desc[0] == 0)
		errx(1, "descriptor length is zero, nothing was read");
	if (desc[1] != UFSHCI_DESC_TYPE_DEVICE)
		errx(1, "descriptor type is 0x%02x, expected 0x00", desc[1]);

	if (negative_cases(fd) != 0)
		errx(1, "the driver accepted input it should refuse");

	if (uic_negative_cases(fd) != 0)
		errx(1, "the driver accepted UIC input it should refuse");

	close(fd);
	printf("PASS\n");
	return (0);
}
