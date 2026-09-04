/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Samsung Electronics Co., Ltd. */

#include <sys/ioctl.h>
#include <sys/types.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <dev/ufshci/ufshci_ioctl.h>

#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * The CAM headers above pull in scsi_all.h, which names these two SCSI
 * opcodes as well. The values agree, so drop them and let ufs-utils
 * define its own. ufs.h reaches scsi_bsg_util.h, so this has to come
 * before it.
 */
#undef SECURITY_PROTOCOL_IN
#undef SECURITY_PROTOCOL_OUT

#include "freebsd_transport.h"
#include "ufs.h"
#include "scsi_bsg_util.h"
#include "options.h"
#include "unipro.h"

/*
 * ufs-utils hands over a 32 byte UPIU and expects the data segment to
 * start right after it. Catch a kernel layout change here rather than
 * letting a descriptor write land at the wrong offset.
 */
_Static_assert(offsetof(struct ufshci_query_request_upiu, command_data) ==
	sizeof(struct utp_upiu_req),
	       "query request data segment must follow the bsg UPIU");
_Static_assert(offsetof(struct ufshci_query_response_upiu, command_data) ==
	sizeof(struct utp_upiu_req),
	       "query response data segment must follow the bsg UPIU");
_Static_assert(sizeof(struct ufshci_cmd_command_upiu) ==
	sizeof(struct utp_upiu_req),
	       "command UPIU and bsg UPIU must be the same size");
_Static_assert(sizeof(struct utp_upiu_req) + sizeof(struct ufs_ehs) <=
	sizeof(struct ufshci_upiu),
	       "an advanced RPMB UPIU and its EHS must fit in one UPIU");

/*
 * Wire structure sizes, as Linux produces them with its own linux/types.h.
 * The compat headers have to reproduce these exactly or the port talks a
 * different protocol.
 */
#define WIRE_SIZE(t, n) \
	_Static_assert(sizeof(t) == (n), #t " is not " #n " bytes")

WIRE_SIZE(struct utp_upiu_header, 12);
WIRE_SIZE(struct utp_upiu_req, 32);
WIRE_SIZE(struct utp_upiu_query, 20);
WIRE_SIZE(struct ufs_bsg_request, 36);
WIRE_SIZE(struct ufs_bsg_reply, 40);
WIRE_SIZE(struct ufs_ehs, 64);
WIRE_SIZE(struct ufs_rpmb_request, 100);
WIRE_SIZE(struct ufs_rpmb_reply, 104);
WIRE_SIZE(struct rpmb_frame, 512);
WIRE_SIZE(struct uic_command, 16);
WIRE_SIZE(__u8, 1);
WIRE_SIZE(__u16, 2);
WIRE_SIZE(__u32, 4);
WIRE_SIZE(__u64, 8);
WIRE_SIZE(__le64, 8);
WIRE_SIZE(__be64, 8);

/*
 * Linux picks long long for the 64 bit types on every architecture
 * (asm-generic/int-ll64.h), and ufs-utils prints them with %llx. Size
 * alone does not catch a swap to uint64_t, which is the same width but
 * a different type, so check the type itself.
 */
#define WIRE_TYPE(t, want) \
	_Static_assert(_Generic((t)0, want: 1, default: 0), \
	    #t " must be " #want ", as on Linux")

WIRE_TYPE(__u64, unsigned long long);
WIRE_TYPE(__s64, long long);

/*
 * Fixed format sense keeps the key and the codes at bytes 2, 12 and 13.
 * Response codes 0x72 and 0x73 are the descriptor format, which uses
 * bytes 1 to 3. Anything else is reserved or vendor specific and is not
 * read as either. Reading the bytes directly avoids scsi_all.h, which
 * would collide with the SCSI opcodes scsi_bsg_util.h defines.
 */
#define SENSE_KEY_UNIT_ATTENTION 0x06

static void
sense_fields(const uint8_t *sense, uint8_t *key, uint8_t *asc, uint8_t *ascq)
{
	if ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73) {
		*key = sense[1] & 0x0f;
		*asc = sense[2];
		*ascq = sense[3];
	} else {
		*key = sense[2] & 0x0f;
		*asc = sense[12];
		*ascq = sense[13];
	}
}

int
freebsd_send_scsi_cmd(int fd, const __u8 *cdb, void *buf,
		      __u8 cmd_len, __u32 byte_cnt, int dir)
{
	union ccb ccb;
	uint8_t key, asc, ascq;
	int tries;
	uint32_t ccb_flags;

	if (fd < 0 || cdb == NULL || cmd_len == 0 || cmd_len > IOCDBLEN ||
	    (byte_cnt != 0 && buf == NULL)) {
		fprintf(stderr, "%s: wrong parameters\n", __func__);
		return -EINVAL;
	}

	switch (dir) {
	case SG_DXFER_FROM_DEV:
		ccb_flags = CAM_DIR_IN;
		break;
	case SG_DXFER_TO_DEV:
		ccb_flags = CAM_DIR_OUT;
		break;
	case SG_DXFER_NONE:
		/* Nothing moves, so the CCB must not describe a buffer. */
		ccb_flags = CAM_DIR_NONE;
		buf = NULL;
		byte_cnt = 0;
		break;
	default:
		fprintf(stderr, "%s: unknown direction %d\n", __func__, dir);
		return -EINVAL;
	}

	/*
	 * A unit attention is the device saying the world changed, and it
	 * clears once reported. The Linux path goes through the SCSI mid
	 * layer, which retries it, so the first command after a boot or a
	 * reset succeeds there. pass(4) does no recovery unless asked, and
	 * asking brings bus resets along with it, so retry this one
	 * condition and nothing else.
	 */
	for (tries = 0; tries < 2; tries++) {
		memset(&ccb, 0, sizeof(ccb));

		/*
		 * pass(4) fills the path from its own periph, so path_id,
		 * target_id and target_lun stay zero. cbfcnp is a kernel side
		 * callback and is not used from userland.
		 */
		cam_fill_csio(&ccb.csio, 1, NULL, ccb_flags, MSG_SIMPLE_Q_TAG,
		    buf, byte_cnt, sizeof(ccb.csio.sense_data), cmd_len,
		    DEF_TIMEOUT_MSEC);
		memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, cmd_len);
		ccb.ccb_h.func_code = XPT_SCSI_IO;

		if (ioctl(fd, CAMIOCOMMAND, &ccb) < 0) {
			int err = errno;

			fprintf(stderr, "CAMIOCOMMAND failed: %s\n",
			    strerror(err));
			return -err;
		}

		if ((ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
			return 0;
		if ((ccb.ccb_h.status & CAM_AUTOSNS_VALID) == 0)
			break;

		sense_fields((const uint8_t *)&ccb.csio.sense_data, &key,
		    &asc, &ascq);
		if (key != SENSE_KEY_UNIT_ATTENTION)
			break;
	}

	fprintf(stderr,
	    "SCSI command 0x%02x failed, CAM status 0x%x, SCSI status 0x%x\n",
	    cdb[0], ccb.ccb_h.status & CAM_STATUS_MASK, ccb.csio.scsi_status);
	if ((ccb.ccb_h.status & CAM_AUTOSNS_VALID) != 0) {
		sense_fields((const uint8_t *)&ccb.csio.sense_data, &key,
		    &asc, &ascq);
		fprintf(stderr, "  sense key 0x%02x asc 0x%02x ascq 0x%02x\n",
		    key, asc, ascq);
	}

	return -EIO;
}

/*
 * A UIC command is not a UPIU. It goes to the host controller registers,
 * so it has its own ioctl. ufs-utils keeps it in the bsg request anyway
 * because the Linux bsg device offers only one path.
 */
static int
freebsd_send_uic(int fd, struct ufs_bsg_request *bsg_req,
		 struct ufs_bsg_reply *bsg_rsp)
{
	struct ufshci_pt_uic_command uic;
	struct uic_command *req_uc, *rsp_uc;

	req_uc = (struct uic_command *)&bsg_req->upiu_req.uc;
	rsp_uc = (struct uic_command *)&bsg_rsp->upiu_rsp.uc;

	memset(&uic, 0, sizeof(uic));
	uic.cmd.opcode = (uint8_t)req_uc->command;
	uic.cmd.argument1 = req_uc->argument1;
	uic.cmd.argument2 = req_uc->argument2;
	uic.cmd.argument3 = req_uc->argument3;

	if (ioctl(fd, UFSHCI_PASSTHROUGH_UIC, &uic) < 0) {
		int err = errno;

		fprintf(stderr, "UFSHCI_PASSTHROUGH_UIC failed: %s\n", strerror(err));
		bsg_rsp->result = -err;
		return -err;
	}

	/*
	 * unipro.c reads the result from argument2 and the attribute value
	 * from argument3, so put them back where it looks.
	 */
	memset(rsp_uc, 0, sizeof(*rsp_uc));
	rsp_uc->command = req_uc->command;
	rsp_uc->argument1 = req_uc->argument1;
	rsp_uc->argument2 = uic.result;
	rsp_uc->argument3 = uic.cmd.argument3;
	bsg_rsp->result = 0;

	return 0;
}

/*
 * Advanced RPMB sends a command UPIU with an EHS behind it. The RPMB
 * frames travel in the PRDT buffer, not in the UPIU data segment, which
 * is what makes this different from a query.
 */
static int
freebsd_send_arpmb(int fd, void *request_buff, void *reply_buff,
		   __u32 req_buf_len, __u32 reply_buf_len,
		   __u32 data_buf_len, __u8 *data_buf, bool write)
{
	struct ufs_rpmb_request *rpmb_req = request_buff;
	struct ufs_rpmb_reply *rpmb_rsp = reply_buff;
	struct ufshci_pt_command pt;
	uint8_t *ehs;

	memset(&pt, 0, sizeof(pt));

	/* This path reads and writes the RPMB structs, not the bsg ones. */
	if (req_buf_len < sizeof(struct ufs_rpmb_request) ||
	    reply_buf_len < sizeof(struct ufs_rpmb_reply)) {
		fprintf(stderr, "%s: buffer too small\n", __func__);
		return -EINVAL;
	}

	/*
	 * The same shape the Linux path insists on
	 * (ufs_bsg_exec_advanced_rpmb_req): two 32 byte units of EHS, type 1.
	 * Catching it here beats sending a malformed request to the device.
	 */
	if (rpmb_req->ehs_req.blenght != 2 || rpmb_req->ehs_req.lehs_type != 1) {
		fprintf(stderr, "%s: bad EHS, length %u type %u\n", __func__,
		    rpmb_req->ehs_req.blenght, rpmb_req->ehs_req.lehs_type);
		return -EINVAL;
	}

	memcpy(&pt.req_upiu, &rpmb_req->bsg_request.upiu_req,
	    sizeof(struct utp_upiu_req));
	ehs = (uint8_t *)&pt.req_upiu + sizeof(struct utp_upiu_req);
	memcpy(ehs, &rpmb_req->ehs_req, sizeof(struct ufs_ehs));

	if (data_buf_len != 0) {
		pt.buf = data_buf;
		pt.len = data_buf_len;
		pt.flags = write ? UFSHCI_PT_FLAG_DATA_OUT :
		    UFSHCI_PT_FLAG_DATA_IN;
	}

	if (ioctl(fd, UFSHCI_PASSTHROUGH_CMD, &pt) < 0) {
		int err = errno;

		fprintf(stderr, "UFSHCI_PASSTHROUGH_CMD failed: %s\n", strerror(err));
		rpmb_rsp->bsg_reply.result = -err;
		return -err;
	}

	memcpy(&rpmb_rsp->bsg_reply.upiu_rsp, &pt.resp_upiu,
	    sizeof(struct utp_upiu_req));

	/*
	 * Take the EHS only when the response says it carries one, the same
	 * test Linux makes. Without an EHS the sense data sits where the EHS
	 * would be, and the caller reads that struct as an RPMB result.
	 */
	if (pt.resp_upiu.header.ehs_length == 2) {
		ehs = (uint8_t *)&pt.resp_upiu + sizeof(struct utp_upiu_req);
		memcpy(&rpmb_rsp->ehs_rsp, ehs, sizeof(struct ufs_ehs));
	}

	rpmb_rsp->bsg_reply.result = 0;
	rpmb_rsp->bsg_reply.reply_payload_rcv_len = pt.xfer_len;

	return 0;
}

int
freebsd_send_bsg_trs(int fd, void *request_buff, void *reply_buff,
		     __u32 req_buf_len, __u32 reply_buf_len,
		     __u32 data_buf_len, __u8 *data_buf, bool write)
{
	struct ufs_bsg_request *bsg_req = request_buff;
	struct ufs_bsg_reply *bsg_rsp = reply_buff;
	struct ufshci_query_request_upiu *q_req;
	struct ufshci_query_response_upiu *q_rsp;
	struct ufshci_pt_command pt;
	size_t copy_len;

	if (bsg_req == NULL || bsg_rsp == NULL) {
		fprintf(stderr, "%s: wrong parameters\n", __func__);
		return -EINVAL;
	}
	if (data_buf_len != 0 && data_buf == NULL) {
		fprintf(stderr, "%s: data_buf is NULL\n", __func__);
		return -EINVAL;
	}
	/*
	 * The UPIU fields are fixed size. Check the buffers hold them rather
	 * than deriving a copy length from the caller's numbers.
	 */
	if (req_buf_len < sizeof(struct ufs_bsg_request) ||
	    reply_buf_len < sizeof(struct ufs_bsg_reply)) {
		fprintf(stderr, "%s: buffer too small\n", __func__);
		return -EINVAL;
	}

	/* The Linux path clears this before it dispatches. Do the same. */
	bsg_rsp->reply_payload_rcv_len = 0;

	switch (bsg_req->msgcode) {
	case UPIU_TRANSACTION_UIC_CMD:
		return freebsd_send_uic(fd, bsg_req, bsg_rsp);
	case UPIU_TRANSACTION_ARPMB_CMD:
		return freebsd_send_arpmb(fd, request_buff, reply_buff,
		    req_buf_len, reply_buf_len, data_buf_len, data_buf, write);
	case UPIU_TRANSACTION_QUERY_REQ:
		break;
	default:
		/*
		 * Falling through to the query path would send whatever this
		 * is as a query. ufs_bsg_request refuses an unknown msgcode
		 * and so does this.
		 */
		fprintf(stderr, "%s: unsupported msgcode 0x%x\n", __func__,
		    bsg_req->msgcode);
		return -ENOTSUP;
	}

	memset(&pt, 0, sizeof(pt));

	memcpy(&pt.req_upiu, &bsg_req->upiu_req, sizeof(bsg_req->upiu_req));

	/*
	 * A query request keeps its data inside the UPIU, not in a separate
	 * buffer, so buf and len stay zero. Move the caller's bytes into the
	 * request UPIU here. On Linux the bsg layer does this in the kernel.
	 */
	if (write && data_buf_len != 0) {
		q_req = (struct ufshci_query_request_upiu *)&pt.req_upiu;
		/*
		 * prepare_upiu already told the device how many bytes are
		 * coming. Sending fewer than that would leave it reading
		 * padding, so refuse instead of truncating.
		 */
		if (data_buf_len > sizeof(q_req->command_data)) {
			fprintf(stderr,
			    "%s: %u bytes to write, the UPIU holds %zu\n",
			    __func__, data_buf_len,
			    sizeof(q_req->command_data));
			return -EINVAL;
		}
		memcpy(q_req->command_data, data_buf, data_buf_len);
	}

	if (ioctl(fd, UFSHCI_PASSTHROUGH_CMD, &pt) < 0) {
		int err = errno;

		fprintf(stderr, "UFSHCI_PASSTHROUGH_CMD failed: %s\n", strerror(err));
		bsg_rsp->result = -err;
		return -err;
	}

	/*
	 * A zero return only means the device answered. do_query_rq reads
	 * the response code out of the UPIU header and decodes it.
	 */
	memcpy(&bsg_rsp->upiu_rsp, &pt.resp_upiu, sizeof(bsg_rsp->upiu_rsp));

	bsg_rsp->result = 0;

	/* And back out of the response UPIU for a read. */
	if (!write && data_buf_len != 0) {
		q_rsp = (struct ufshci_query_response_upiu *)&pt.resp_upiu;
		/*
		 * Take the length from the UPIU header. That is how much data
		 * the device actually sent. The Length field beside the
		 * opcode can echo what was asked for instead.
		 */
		copy_len = be16toh(q_rsp->header.data_segment_length);
		if (copy_len > sizeof(q_rsp->command_data)) {
			fprintf(stderr,
			    "%s: device claims %zu bytes, the UPIU holds %zu\n",
			    __func__, copy_len, sizeof(q_rsp->command_data));
			return -EINVAL;
		}
		/*
		 * Refuse rather than truncate, the way the Linux path does
		 * (ufshcd_issue_devman_upiu_cmd). A short descriptor parsed
		 * as a whole one gives wrong values and says nothing.
		 */
		if (copy_len > data_buf_len) {
			fprintf(stderr,
			    "%s: response is %zu bytes, the buffer holds %u\n",
			    __func__, copy_len, data_buf_len);
			bsg_rsp->reply_payload_rcv_len = 0;
			return -EINVAL;
		}
		memcpy(data_buf, q_rsp->command_data, copy_len);
		bsg_rsp->reply_payload_rcv_len = copy_len;
	}

	return 0;
}
