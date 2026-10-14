#!/usr/bin/env python3
"""Offsets in libFEXInversa.so -> function+offset.  fexsym.py <offset>..."""
import bisect, os, re, subprocess, sys
lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "lib", "libFEXInversa.so")
nm = os.path.expanduser("~/wine-arm64/tc/llvm-mingw-20260922-ucrt-macos-universal/bin/llvm-nm")
syms = []
for l in subprocess.run([nm, "-C", "--defined-only", lib], capture_output=True, text=True).stdout.splitlines():
    p = l.split(" ", 2)
    if len(p) == 3 and re.fullmatch(r"[0-9a-f]+", p[0]): syms.append((int(p[0], 16), p[2]))
syms.sort(); keys = [s[0] for s in syms]
for a in sys.argv[1:]:
    v = int(a, 16); i = bisect.bisect_right(keys, v) - 1
    print("%-8s %s+%#x" % (a, syms[i][1][:160], v - syms[i][0]))
