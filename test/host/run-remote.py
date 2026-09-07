#!/usr/bin/env python3
"""Push this suite to a target, optionally build the module, and run it.

    ./host/run-remote.py qemu
    ./host/run-remote.py qemu --build 300-error-recovery
    ./host/run-remote.py gb --build

--build writes down what it built. The suite then refuses to load a
module whose record no longer matches the source tree, which is what
catches a tree that moved between the build and the load. Getting that
wrong once cost two days of a corrupted device attribute on the Galaxy
Book, so it lives in the tool rather than in a procedure to remember.

Deciding which branch to test is still outside. Pass --expect-commit to
have the suite check that too.
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import target as tgt  # noqa: E402

TOPDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REMOTE_DIR = "/root/ufshci-tests"


def deploy(name):
    """Copy the suite to the target as a tarball."""
    fd, tarball = tempfile.mkstemp(suffix=".tar")
    os.close(fd)
    # Bytecode compiled here has no business on the target. Python would
    # ignore it on a version mismatch anyway, and shipping it only makes
    # the tarball bigger and the target's tree less obviously a copy.
    subprocess.check_call([
        "tar", "-C", TOPDIR, "--exclude=.git", "--exclude=logs",
        "--exclude=__pycache__", "--exclude=*.pyc",
        "-cf", tarball, "."])

    print("pushing the suite to %s" % name)
    if not tgt.scp_push(name, tarball, "/tmp/ufshci-tests.tar"):
        os.unlink(tarball)
        sys.exit("could not copy the suite to %s" % name)
    os.unlink(tarball)

    cmd = ("rm -rf %s && mkdir -p %s && "
           "tar -C %s -xf /tmp/ufshci-tests.tar && "
           "rm -f /tmp/ufshci-tests.tar && echo DEPLOYED"
           % (REMOTE_DIR, REMOTE_DIR, REMOTE_DIR))
    out, timed_out = tgt.ssh_run(name, cmd, timeout=120, stream=False)
    if timed_out or "DEPLOYED" not in out:
        sys.exit("unpacking the suite failed:\n%s" % out)


def find_python(name):
    """Find an interpreter on the target.

    FreeBSD installs the port as python3.12 and only the python3 meta
    port adds the plain python3 name, so both have to be looked for.
    """
    probe = ("for p in python3 python3.12 python3.11 python3.10; do "
             "command -v $p && break; done")
    out, timed_out = tgt.ssh_run(name, probe, timeout=60, stream=False)
    if timed_out:
        sys.exit("could not probe %s for a python interpreter" % name)
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("/") and "python3" in line:
            return line
    sys.exit("no python3 on %s, install it with: pkg install python3" % name)


def build(name, python):
    print("building the module on %s" % name)
    out, timed_out = tgt.ssh_run(
        name, "make -C /usr/src/sys/modules/ufshci clean all 2>&1 | tail -5",
        timeout=900)
    if timed_out:
        sys.exit("the build timed out")
    if "Error" in out or "error:" in out:
        sys.exit("the build failed:\n%s" % out)

    # Write down what was built while it is still true. The load path
    # refuses a module whose record no longer matches the tree, which is
    # what catches a tree that moved between the build and the load.
    #
    # Invoked as a module, not with python -c: the Galaxy Book wraps
    # commands for su, and a quote inside one breaks that wrapping.
    out, timed_out = tgt.ssh_run(
        name, "cd %s && %s -m ufshci.integrity" % (REMOTE_DIR, python),
        timeout=120, stream=False)
    if timed_out or "MARKER_OK" not in out:
        sys.exit("built the module but could not record what it was built "
                 "from:\n%s" % out)
    print("recorded the build")


def pull_results(name, local_dir):
    """Bring the measurement history back so the host accumulates it.

    Each target keeps its own file, because a number from the Galaxy Book
    and one from an emulated controller do not belong in the same series.
    """
    os.makedirs(local_dir, exist_ok=True)
    local = os.path.join(local_dir, "%s.tsv" % name)
    if tgt.scp_pull(name, "/var/tmp/ufshci-tests/results.tsv", local):
        print("results history copied to %s" % local)
    else:
        print("no results history on %s yet" % name)


def run(name, python, script, args, timeout):
    cmd = "%s %s/%s %s; echo RC=$?" % (python, REMOTE_DIR, script,
                                       " ".join(args))
    out, timed_out = tgt.ssh_run(name, cmd, timeout=timeout)
    if timed_out:
        print("\nthe run timed out, the output above is partial")
        return 124
    for line in out.splitlines():
        if line.startswith("RC="):
            return int(line[3:].strip())
    print("\nno exit status came back, treating this as a failure")
    return 1


def main():
    p = argparse.ArgumentParser(
        epilog="Anything not listed above is passed through to run.py, "
               "so flags like -e and -l work here too.")
    p.add_argument("target", choices=sorted(tgt.TARGETS))
    p.add_argument("--build", action="store_true",
                   help="rebuild the module on the target first")
    p.add_argument("--bench", action="store_true",
                   help="run bench.py instead of the test suite")
    p.add_argument("--timeout", type=int, default=1800)
    # Pass the rest through untouched. argparse cannot hold a variable
    # number of trailing words next to optional flags without tripping
    # over them, and run.py already knows what to do with its own flags.
    args, passthrough = p.parse_known_args()
    passthrough = [a for a in passthrough if a != "--"]

    deploy(args.target)
    python = find_python(args.target)
    if args.build:
        build(args.target, python)

    script = "bench.py" if args.bench else "run.py"
    rc = run(args.target, python, script, passthrough, args.timeout)
    if args.bench:
        pull_results(args.target, os.path.join(TOPDIR, "results"))
    sys.exit(rc)


if __name__ == "__main__":
    main()
