/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Samsung Electronics Co., Ltd. */
/*
 * FreeBSD replacements for the two transport functions in
 * scsi_bsg_util.c. Those two are the only places ufs-utils talks to the
 * kernel.
 */

#ifndef UFS_UTILS_FREEBSD_TRANSPORT_H_
#define UFS_UTILS_FREEBSD_TRANSPORT_H_

#include <stdbool.h>
#include <linux/types.h>

/* Sends a SCSI CDB. dir is SG_DXFER_TO_DEV or SG_DXFER_FROM_DEV. */
int freebsd_send_scsi_cmd(int fd, const __u8 *cdb, void *buf,
			  __u8 cmd_len, __u32 byte_cnt, int dir);

/* Sends a raw UPIU. Reads the message type from the request msgcode. */
int freebsd_send_bsg_trs(int fd, void *request_buff, void *reply_buff,
			 __u32 req_buf_len, __u32 reply_buf_len,
			 __u32 data_buf_len, __u8 *data_buf, bool write);

#endif /* UFS_UTILS_FREEBSD_TRANSPORT_H_ */
