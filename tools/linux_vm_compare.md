# Comparing the FreeBSD transport against Linux

The port swaps two functions in ufs-utils. Everything above them is
shared. So the same ufs-utils built on Linux and on FreeBSD, run against
the same device, should print the same thing. This is how that was
checked without a Linux UFS device: a Linux VM with QEMU's emulated UFS.

## Kernel

Build a Linux kernel with UFS and the bsg passthrough. From a Linux
source tree:

    make O=$O x86_64_defconfig
    scripts/config --file $O/.config \
      -e SCSI -e BLK_DEV_SD -e CHR_DEV_SG \
      -e SCSI_UFSHCD -e SCSI_UFSHCD_PCI -e SCSI_UFS_BSG \
      -e BLK_DEV_BSG_COMMON -e BLK_DEV_BSGLIB \
      -e DEVTMPFS -e DEVTMPFS_MOUNT -e BLK_DEV_INITRD \
      -e SERIAL_8250 -e SERIAL_8250_CONSOLE -e DEBUG_INFO_NONE \
      -d MODULE_SIG -d SECURITY_LOCKDOWN_LSM
    make O=$O olddefconfig && make O=$O -j bzImage

## Root filesystem

A busybox initramfs. Do not symlink busybox to itself: `busybox --list`
includes `busybox`, and the self-link overwrites the real binary and
boots to ELOOP. Filter it out:

    for a in $(busybox --list | grep -vx busybox); do ln -sf busybox bin/$a; done

Put the Linux ufs-utils and tools/compare_query.sh in the image. The init
script mounts devtmpfs, then runs compare_query.sh against the bsg node.

## Device

    -device ufs -drive file=lu.bin,if=none,id=lu,format=raw \
    -device ufs-lu,drive=lu,lun=0

The bsg node is /dev/bsg/ufs-bsgN, not ufs_bsg0. Read the name from an
`ls /dev/bsg` in the init script.

## What to expect

QEMU prints its own device traces on the same serial console. Strip lines
that start with the trace marker before diffing. Then the descriptors a
device supports match byte for byte. Two things do not, both explained in
PLAN.md: bRefClkFreq (driver init writes it) and an unsupported
descriptor (Linux hides the query error, FreeBSD reports it).
