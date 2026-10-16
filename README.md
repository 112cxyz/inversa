# Inversa

Run Linux programs on Apple silicon Macs, without a VM:

- **Linux ARM64** programs natively, loaded by [Klepton](https://github.com/shinyquagsire23/Klepton).
- **Linux x86-64** programs through [FEX](https://github.com/FEX-Emu/FEX), itself built for Linux ARM64 and loaded the same way.

Both sit on Inversa's own kernel layer (`kernel/`), which implements the Linux syscall ABI on top of Darwin.

```
inversa run <program> [args...]
inversa probe <program>          # load it and list what doesn't resolve
```

## Building

Needs Xcode's command-line tools and Homebrew (`brew install llvm lld ninja cmake vulkan-headers`).

```
tools/setup.sh     # FEX and Klepton at their pinned commits, with patches/ applied
make sysroot       # Debian arm64 sysroot for cross-building FEX
make fex           # build/lib/libFEXInversa.so
make               # build/Inversa.app
make rootfs        # build/rootfs/x86_64 (bash, coreutils) and the arm64 libraries FEX links against
```

`build/Inversa.app` is signed with a Developer ID and the cross-architecture entitlement when both are available (`SIGN_ID`, `PROFILE`). That gives the process 4K pages and memory below 4 GB, which Linux programs expect. Otherwise it's signed ad hoc.

An x86-64 run now exits before loading FEX if the process still has 16K pages. For an experimental guest run, use `python3 tools/safe_run.py --timeout 10 --log /tmp/inversa-test.log -- build/Inversa.app/Contents/MacOS/inversa run <program> [args...]` from an environment where the watchdog can signal the process group. The runner limits CPU time and attempts to terminate the process group on exit or timeout. If group signaling is denied, it kills the direct child and reports a cleanup error; any descendants need separate inspection. A watchdog cannot protect against a macOS kernel panic; investigate JIT or VM faults in a disposable VM first.

The experimental ARM64 loader that runs a program with its own Linux `ld.so` and glibc is enabled with `INVERSA_ARM64_LINUX=1`. It expects an ARM64 root at `build/rootfs/aarch64` or `INVERSA_ROOTFS`; the Klepton path remains the default.

## Layout

| | |
|---|---|
| `host/` | the `inversa` command: picks the path by ELF machine, feeds FEX the CPU's features |
| `kernel/` | Linux syscalls on Darwin: memory, files (`openat2` inside a root), stat layouts, futexes, signals, threads |
| `loader/` | the glibc ABI over Darwin libc, ELF TLS, JIT memory, crash reports |
| `third_party/` | FEX and Klepton (submodules); `patches/` holds what Inversa changes in them |
| `tools/` | Debian root/sysroot fetcher, cross toolchain file, symbolizer |

Debugging: `INVERSA_STRACE=1` (host-side syscalls), `INVERSA_GSTRACE=1` (the guest's x86-64 syscalls), `MVR_CRASH_REGS=1` / `MVR_CRASH_STACK=1` (more in crash reports).
