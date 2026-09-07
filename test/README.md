# FreeBSD passthrough test tooling

Scaffolding for testing the FreeBSD ufs-utils port against the ufshci
kernel driver. It is kept off the `freebsd` branch so the upstream PR
stays clean. None of this is built or shipped by the port.

## Layout

- `../fb` — push the tree to a target, build it there, run the tools.
- `../tools/` — the test programs, described below.
- `host/target.py.example` — the ssh/scp harness. It holds hosts and
  passwords, so only the example is committed.
- `host/run-remote.py` — a thin runner over the harness.

## Setup

    cp test/host/target.py.example test/host/target.py
    # edit test/host/target.py: set each target's host and password

`target.py` is gitignored so real credentials never get committed. The
targets use password auth, reached through a pexpect wrapper.

## Use

    ./fb build              # push, then gmake on the target
    ./fb test               # build and run the tools tests
    UFS_TARGET=gb ./fb run '<cmd>'   # pick a target, default is qemu

## Tools

- `ufshci_pt_test.c` — read a descriptor and send a SCSI INQUIRY through
  the passthrough ioctl. Prints PASS or FAIL.
- `pass_inquiry_test.c` — the INQUIRY path on its own.
- `layout_test.c` — assert the passthrough struct offsets.
- `pt_fuzz.c` — feed the ioctl random requests. `-s` is safe mode: it
  clears the write bit and steers away from anything that changes device
  state, so it is safe on real hardware. Run it there in safe mode only.
- `arpmb_dump.c` — dump an advanced RPMB exchange.
- `compare_query.sh` — compare query output against Linux.
- `linux_vm_compare.md` — notes from the Linux comparison.
