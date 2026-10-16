#!/usr/bin/env python3
"""Run a potentially stuck guest with bounded CPU time and process-group cleanup.

Usage: python3 tools/safe_run.py --timeout 10 --log /tmp/inversa-test.log -- COMMAND ...
Run this outside a restrictive sandbox: the watchdog must be allowed to signal its child.
"""

import argparse
import os
import resource
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--cpu-seconds", type=int, default=5)
    parser.add_argument("--log", required=True)
    parser.add_argument("--sample-on-timeout", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or args.timeout <= 0 or args.cpu_seconds <= 0:
        parser.error("a command and positive limits are required")

    def limits():
        resource.setrlimit(resource.RLIMIT_CPU, (args.cpu_seconds, args.cpu_seconds + 1))
        resource.setrlimit(resource.RLIMIT_FSIZE, (16 << 20, 16 << 20))

    with open(args.log, "wb") as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True,
                                   preexec_fn=limits)
        timed_out = False
        cleanup_error = None

        def members():
            """Include orphaned descendants that still have the child's process group."""
            output = subprocess.check_output(["ps", "-axo", "pid=,ppid=,pgid="], text=True)
            rows = [tuple(map(int, line.split())) for line in output.splitlines() if line.strip()]
            found = {pid for pid, _, pgid in rows if pgid == process.pid}
            found.add(process.pid)
            while True:
                more = {pid for pid, ppid, _ in rows if ppid in found}
                if more <= found:
                    return found
                found |= more

        def signal_members(sig):
            errors = []
            for pid in sorted(members(), key=lambda pid: pid == process.pid):
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    pass
                except PermissionError as exc:
                    errors.append(f"{pid}: {exc}")
            return errors

        try:
            process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            if args.sample_on_timeout:
                for pid in sorted(members()):
                    if pid == process.pid:
                        continue
                    try:
                        subprocess.run(["sample", str(pid), "1", "1", "-file", f"{args.log}.{pid}.sample"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=4)
                    except (subprocess.SubprocessError, OSError):
                        pass
        finally:
            # macOS may deny killpg even for our own session; signal its members directly.
            errors = signal_members(signal.SIGTERM)
            time.sleep(0.2)
            errors += signal_members(signal.SIGKILL)
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                cleanup_error = TimeoutError("process did not exit after SIGKILL")
            if errors:
                cleanup_error = "; ".join(errors)
            survivors = [pid for pid in members() if pid != process.pid]
            if survivors:
                cleanup_error = f"processes still alive: {survivors}"

    if cleanup_error:
        print(f"watchdog could not clean up session {process.pid}: {cleanup_error}", file=sys.stderr)
        return 125
    if timed_out:
        print(f"timed out after {args.timeout:g}s; session {process.pid} stopped; log: {args.log}",
              file=sys.stderr)
        return 124
    return process.returncode if process.returncode is not None else 125


if __name__ == "__main__":
    sys.exit(main())
