#!/bin/sh
# Run the same read-only query commands on Linux and FreeBSD, so their
# output can be diffed. $1 is the query device path: /dev/bsg/ufs-bsgN on
# Linux, /dev/ufshci0 on FreeBSD. UFS_UTILS overrides the binary.
#
# The point is a byte-for-byte match on the descriptors a device supports.
# A read of an unsupported descriptor is where the two paths part: see the
# Linux comparison in PLAN.md.
DEV=$1
U=${UFS_UTILS:-ufs-utils}
run() { echo "### $*"; $U "$@" 2>&1; echo "### rc=$?"; }
for t in 0 1 2 4 5 7 8 9; do run desc -t $t -p $DEV; done
run desc -t 0x7f -p $DEV
run attr -a -p $DEV
run attr -t 0x0a -p $DEV
run attr -t 0x7f -p $DEV
run fl -a -p $DEV
run fl -t 0x7f -p $DEV
run spec_version -p $DEV
run uic -t 1 -r -i 0x1568 -p $DEV
run list_bsg
echo "### END"
