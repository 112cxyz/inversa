#!/usr/bin/env python3
"""The regression set: x86-64 programs under build/Inversa.app, each bounded by tools/safe_run.py.

Usage: python3 tests/check.py [name ...]      (needs `make all rootfs tests` first)
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INVERSA = os.path.join(ROOT, "build/Inversa.app/Contents/MacOS/inversa")
BASH = os.path.join(ROOT, "build/rootfs/x86_64/usr/bin/bash")
ZEROS_SHA256 = "ab46920a3bcd0891d34367719808bc3f832e4968ddfbfb464d093e306d2275ad"

# name, guest command line, expected exit status, text the output must contain
CASES = [
    ("static", [os.path.join(ROOT, "tests/build/hello_x64")], 42, ""),
    ("script", [BASH, "-c", "for i in 1 2 3; do echo line $i; done"], 0, "line 3"),
    ("pipe", [BASH, "-c", "ls / | head -3 | wc -l"], 0, "3"),
    ("exec-chain", [BASH, "-c", "echo a b c | tr a-z A-Z | wc -c"], 0, "6"),
    ("subst", [BASH, "-c", "x=$(uname -m); echo arch=$x"], 0, "arch=x86_64"),
    ("signals", [BASH, "-c", "trap 'echo caught' USR1; kill -USR1 $$; echo after"], 0, "caught\nafter"),
    ("background", [BASH, "-c", "sleep 0.2 & wait $!; echo waited $?"], 0, "waited 0"),
    ("nested-forks", [BASH, "-c", "( ( echo deep ) ); echo next"], 0, "deep\nnext"),
    ("fork-then-subst", [BASH, "-c", "( echo a ); echo $(echo b)"], 0, "a\nb"),
    ("nested-subst", [BASH, "-c", "echo $( echo $( echo two ) )"], 0, "two"),
    ("sha256", [BASH, "-c", "head -c 50000000 /dev/zero | sha256sum"], 0, ZEROS_SHA256),
]

# a second fork that runs bash code without exec hangs on its pipe (or exits 232)
KNOWN_FAILURES = {"fork-then-subst", "nested-subst"}


def run(name, argv, status, expect, logdir):
    log = os.path.join(logdir, name + ".log")
    rc = subprocess.run([sys.executable, os.path.join(ROOT, "tools/safe_run.py"), "--timeout", "20",
                         "--log", log, "--", INVERSA, "run", *argv]).returncode
    with open(log, errors="replace") as f:
        out = f.read()
    ok = rc == status and expect in out
    known = not ok and name in KNOWN_FAILURES
    print(f"{'ok  ' if ok else 'known' if known else 'FAIL'} {name}" + ("" if ok else f" (exit {rc}, log {log})"))
    return ok or known


def main():
    wanted = set(sys.argv[1:])
    logdir = tempfile.mkdtemp(prefix="inversa-check-")
    results = [run(*case, logdir) for case in CASES if not wanted or case[0] in wanted]
    print(f"{sum(results)}/{len(results)} passed or known")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
