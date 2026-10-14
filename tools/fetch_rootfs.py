#!/usr/bin/env python3
"""A Linux root filesystem for Inversa from Debian packages and their dependencies.
  fetch_rootfs.py <arch> <dir> <package>...      e.g. fetch_rootfs.py amd64 rootfs/x86_64 bash coreutils
Packages are unpacked as is (no maintainer scripts); Debian's merged /usr symlinks are added."""
import io, lzma, os, re, subprocess, sys, tarfile, urllib.request

MIRROR, SUITE = "https://deb.debian.org/debian", "trixie"
arch, out, wanted = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3:]
dl = os.path.join(os.path.dirname(out), ".debs", arch)
os.makedirs(dl, exist_ok=True)

def index():
    path = os.path.join(dl, "Packages.xz")
    if not os.path.exists(path):
        urllib.request.urlretrieve("%s/dists/%s/main/binary-%s/Packages.xz" % (MIRROR, SUITE, arch), path)
    pkgs, provides, cur = {}, {}, {}
    for line in lzma.open(path, "rt", errors="ignore"):
        line = line.rstrip("\n")
        if not line:
            if "Package" in cur:
                pkgs[cur["Package"]] = cur
                for p in re.split(r",\s*", cur.get("Provides", "")):
                    if p: provides.setdefault(p.split()[0], cur["Package"])
            cur = {}
        elif ": " in line and not line.startswith(" "):
            k, v = line.split(": ", 1); cur[k] = v
    return pkgs, provides

pkgs, provides = index()
# not wanted in a guest root: init and package machinery
SKIP = {"debconf", "dpkg", "perl-base", "init-system-helpers", "debianutils", "base-files", "sensible-utils"}

def resolve(names):
    seen, todo = set(), list(names)
    while todo:
        n = todo.pop()
        n = n if n in pkgs else provides.get(n)
        if not n or n in seen or n in SKIP: continue
        seen.add(n)
        for field in ("Pre-Depends", "Depends"):
            for dep in re.split(r",\s*", pkgs[n].get(field, "")):
                if dep: todo.append(dep.split("|")[0].strip().split()[0].split(":")[0])
    return sorted(seen)

def unpack(name):
    p = pkgs[name]
    deb = os.path.join(dl, os.path.basename(p["Filename"]))
    if not os.path.exists(deb): urllib.request.urlretrieve(MIRROR + "/" + p["Filename"], deb)
    member = [m for m in subprocess.run(["ar", "t", deb], capture_output=True, text=True).stdout.split() if m.startswith("data.tar")][0]
    data = subprocess.run(["ar", "p", deb, member], capture_output=True).stdout
    if member.endswith(".zst"): data = subprocess.run(["zstd", "-dc"], input=data, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*" if not member.endswith(".zst") else "r:") as t:
        for m in t.getmembers():
            name_ = m.name[2:] if m.name.startswith("./") else m.name
            if name_.startswith(("usr/share/doc", "usr/share/man", "usr/share/info", "usr/share/locale")): continue
            dest = os.path.join(out, name_)
            if m.isdir(): os.makedirs(dest, exist_ok=True)
            elif m.issym() or m.islnk():
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if os.path.lexists(dest): os.remove(dest)
                if m.issym(): os.symlink(m.linkname, dest)
                else: os.link(os.path.join(out, m.linkname[2:] if m.linkname.startswith("./") else m.linkname), dest)
            elif m.isfile():
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if os.path.lexists(dest): os.remove(dest)
                with open(dest, "wb") as f: f.write(t.extractfile(m).read())
                os.chmod(dest, m.mode & 0o7777 | 0o200)

os.makedirs(out, exist_ok=True)
for d in ("bin", "sbin", "lib", "lib64"):   # merged /usr
    os.makedirs(os.path.join(out, "usr", d), exist_ok=True)
    if not os.path.lexists(os.path.join(out, d)): os.symlink("usr/" + d, os.path.join(out, d))
for n in resolve(wanted):
    unpack(n); print(n, pkgs[n]["Version"])
for d in ("etc", "tmp", "proc", "dev", "home", "root", "var/tmp"): os.makedirs(os.path.join(out, d), exist_ok=True)
