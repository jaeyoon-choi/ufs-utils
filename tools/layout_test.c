/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The port swaps the Linux kernel headers for the ones in compat/. Every
 * structure below crosses the kernel boundary, so a size that drifts
 * from the Linux layout is a wire format bug, not a style problem.
 *
 * The numbers are what Linux produces with its own linux/types.h. Build
 * this on both platforms.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ufs.h"
#include "options.h"
#include "scsi_bsg_util.h"
#include "unipro.h"

#define CHECK(t, n) \
	_Static_assert(sizeof(t) == (n), #t " is not " #n " bytes")

CHECK(struct utp_upiu_header, 12);
CHECK(struct utp_upiu_req, 32);
CHECK(struct utp_upiu_query, 20);
CHECK(struct ufs_bsg_request, 36);
CHECK(struct ufs_bsg_reply, 40);
CHECK(struct ufs_ehs, 64);
CHECK(struct ufs_rpmb_request, 100);
CHECK(struct ufs_rpmb_reply, 104);
CHECK(struct rpmb_frame, 512);
CHECK(struct uic_command, 16);

/*
 * Size does not pin the 64 bit types down: uint64_t and unsigned long
 * long are both eight bytes. ufs-utils prints them with %llx, so the
 * type has to match what Linux picks, not just the width.
 */
#define CHECK_TYPE(t, want) \
	_Static_assert(_Generic((t)0, want: 1, default: 0), \
	    #t " must be " #want ", as on Linux")

CHECK_TYPE(__u64, unsigned long long);
CHECK_TYPE(__s64, long long);

CHECK(__u8, 1);
CHECK(__u16, 2);
CHECK(__u32, 4);
CHECK(__u64, 8);
CHECK(__le64, 8);
CHECK(__be64, 8);

int
main(void)
{
	printf("PASS\n");
	return (0);
}
