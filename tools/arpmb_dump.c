/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Dump the raw bytes of an Advanced RPMB read counter exchange.
 *
 * ufs-utils reads the result out of the response EHS. If the EHS never
 * came back, the zeroed struct reads as success with a counter of zero,
 * which is indistinguishable from a real answer. This prints what the
 * device actually returned so the two can be told apart.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <err.h>
#include <fcntl.h>
#include <unistd.h>

#include "ufs.h"
#include "options.h"
#include "scsi_bsg_util.h"

#define SEC_PROTOCOL_CMD_SIZE 12
#define RPMB_READ_CNT 0x0002

static void
dump(const char *what, const void *p, size_t n)
{
	const uint8_t *b = p;
	size_t i;

	printf("%s (%zu bytes):\n", what, n);
	for (i = 0; i < n; i++) {
		printf("%02x%s", b[i],
		    ((i + 1) % 16 == 0 || i + 1 == n) ? "\n" : " ");
	}
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/ufshci0";
	struct ufs_rpmb_request req;
	struct ufs_rpmb_reply rsp;
	uint8_t cdb[SEC_PROTOCOL_CMD_SIZE] = { 0 };
	int fd, ret;

	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));

	req.ehs_req.blenght = 0x02;
	req.ehs_req.lehs_type = 0x01;
	req.ehs_req.meta.req_resp_type = htobe16(RPMB_READ_CNT);

	prepare_security_cdb(cdb, 0, 0, SECURITY_PROTOCOL_IN);
	prepare_command_upiu(&req.bsg_request.upiu_req, 0x40, 0xC4, 2, cdb,
	    SEC_PROTOCOL_CMD_SIZE, 0);
	req.bsg_request.msgcode = UPIU_TRANSACTION_ARPMB_CMD;

	fd = open(path, O_RDWR);
	if (fd < 0)
		err(1, "open %s", path);

	ret = send_bsg_scsi_trs(fd, &req, &rsp, sizeof(req), sizeof(rsp), 0,
	    NULL, false);
	printf("send_bsg_scsi_trs returned %d\n", ret);
	close(fd);

	dump("request UPIU", &req.bsg_request.upiu_req,
	    sizeof(struct utp_upiu_req));
	dump("request EHS", &req.ehs_req, sizeof(struct ufs_ehs));
	dump("response UPIU", &rsp.bsg_reply.upiu_rsp,
	    sizeof(struct utp_upiu_req));
	dump("response EHS", &rsp.ehs_rsp, sizeof(struct ufs_ehs));

	printf("ehs blenght=%u lehs_type=%u result=0x%04x counter=%u\n",
	    rsp.ehs_rsp.blenght, rsp.ehs_rsp.lehs_type,
	    be16toh(rsp.ehs_rsp.meta.result),
	    be32toh(rsp.ehs_rsp.meta.write_counter));
	return (0);
}
