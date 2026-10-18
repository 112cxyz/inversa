#!/usr/bin/env python3
"""steamcmd on macOS writes a Linux depot's symlinks as empty files. Recreate them: an empty
libX.so points at its versioned sibling, and every versioned library gets a link named after
its SONAME.  fix_depot_links.py <dir>..."""
import os, re, subprocess, sys

llvm = subprocess.run(["brew", "--prefix", "llvm"], capture_output=True, text=True).stdout.strip()
READELF = os.environ.get("LLVM_READELF") or os.path.join(llvm, "bin", "llvm-readelf")

for d in sys.argv[1:]:
    files = os.listdir(d)
    for f in files:
        p = os.path.join(d, f)
        if os.path.islink(p) or not os.path.isfile(p) or os.path.getsize(p): continue
        siblings = sorted((s for s in files if s.startswith(f + ".") and os.path.getsize(os.path.join(d, s))), key=len)
        if siblings:
            os.remove(p); os.symlink(siblings[-1], p); print(f"{f} -> {siblings[-1]}")
    for f in os.listdir(d):
        p = os.path.join(d, f)
        if os.path.islink(p) or not os.path.isfile(p) or open(p, "rb").read(4) != b"\x7fELF": continue
        m = re.search(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", subprocess.run([READELF, "-d", p], capture_output=True, text=True).stdout)
        if m and m.group(1) != f and not os.path.exists(os.path.join(d, m.group(1))):
            os.symlink(f, os.path.join(d, m.group(1))); print(f"{m.group(1)} -> {f}")
