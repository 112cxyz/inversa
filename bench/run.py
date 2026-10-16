#!/usr/bin/env python3
"""Run cpubench under every translator and compare.  bench/run.py [rounds]
The binaries come from build/bench (see the README). The Wine rows need ARM64_WINE (an arm64
Wine with FEX for x86-64) and ROSETTA_WINE (an x86-64 Wine); rows whose Wine is missing are skipped."""
import json, os, statistics, subprocess, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build/bench")
INV = os.path.join(ROOT, "build/Inversa.app/Contents/MacOS/inversa")
AW = os.environ.get("ARM64_WINE", "")
RW = os.environ.get("ROSETTA_WINE", "")
APFX = os.environ.get("ARM64_WINEPREFIX", os.path.join(B, "pfx-arm64"))
RPFX = os.environ.get("ROSETTA_WINEPREFIX", os.path.join(B, "pfx-rosetta"))
CONFIGS = [  # name, argv prefix, binary, env
    ("macOS arm64 (native)",          [], "mac-arm64", {}),
    ("macOS x86-64 (Rosetta 2)",      ["arch", "-x86_64"], "mac-x64", {}),
    ("Linux arm64 (Inversa/Klepton)", [INV, "run"], "linux-arm64", {}),
    ("Linux x86-64 (Inversa/FEX)",    [INV, "run"], "linux-x64", {}),
    ("Windows arm64 (arm64 Wine)",    [AW], "win-arm64.exe", {"WINEPREFIX": APFX}),
    ("Windows x64 (arm64 Wine+FEX)",  [AW], "win-x64.exe", {"WINEPREFIX": APFX}),
    ("Windows x64 (Rosetta Wine)",    [RW], "win-x64.exe", {"WINEPREFIX": RPFX}),
]
CONFIGS = [c for c in CONFIGS if not c[1] or c[1][0] == "arch" or os.path.exists(c[1][0])]
TESTS = ["sha256", "nbody", "memcpy", "qsort", "oscall"]

def run(cfg, args):
    name, pre, binary, env = cfg
    e = dict(os.environ, WINEDEBUG="-all", **env)
    t = time.time()
    p = subprocess.run(pre + [os.path.join(B, binary)] + args, capture_output=True, text=True, env=e,
                       stdin=subprocess.DEVNULL, start_new_session=True, timeout=600)
    return time.time() - t, p.stdout

rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
best = {c[0]: {} for c in CONFIGS}
for r in range(rounds):
    for cfg in CONFIGS:
        _, out = run(cfg, [])
        for line in out.splitlines():
            k, _, v = line.partition(" ")
            if k in TESTS: best[cfg[0]][k] = min(best[cfg[0]].get(k, 1e9), float(v))
        print("round %d: %-31s %s" % (r + 1, cfg[0], " ".join("%s=%.3f" % (k, best[cfg[0]].get(k, -1)) for k in TESTS)), flush=True)
# startup: warm wineservers first so it's program start, not Wine booting
for pfx, w in ((APFX, AW), (RPFX, RW)):
    if not w: continue
    server = os.path.join(os.path.dirname(w), "wineserver")
    if not os.path.exists(server): server = os.path.join(os.path.dirname(w), "../Resources/bin/wineserver")   # Wine.app
    if not os.path.exists(server): continue
    subprocess.Popen([server, "-p"],
                     env=dict(os.environ, WINEPREFIX=pfx), start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3)
startup = {}
for cfg in CONFIGS:
    run(cfg, ["nop"])
    startup[cfg[0]] = statistics.median(run(cfg, ["nop"])[0] for _ in range(7))
json.dump({"best": best, "startup": startup}, open(os.path.join(B, "results.json"), "w"), indent=1)
base = best[CONFIGS[0][0]]
print("\n%-31s %s %s" % ("", " ".join("%9s" % t for t in TESTS), "  start-up"))
for cfg in CONFIGS:
    n = cfg[0]
    cells = " ".join("%8.0f%%" % (100 * base[t] / best[n][t]) if t in best[n] else "%9s" % "-" for t in TESTS)
    print("%-31s %s %8.0f ms" % (n, cells, startup[n] * 1000))
print("\n(% = speed relative to native macOS arm64, higher is better; start-up = median wall time of an empty run)")
