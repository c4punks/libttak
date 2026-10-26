#!/usr/bin/env python3
"""Flash bare-metal unit-test images to an STM32 board and collect results.

Each ELF is programmed with OpenOCD, reset, and its console is read from
the ST-LINK virtual COM port until the harness prints "@@TTAK_EXIT=<n>@@".

    make PLATFORM=baremetal BOARD=stm32f769i_disco baremetal-tests
    scripts/baremetal/run_stm32.py tests/baremetal/build/stm32f769i_disco/*.elf

Only the standard library is used (termios for the serial port).
"""

import argparse
import os
import re
import select
import subprocess
import sys
import termios
import time

EXIT_RE = re.compile(rb"@@TTAK_EXIT=(-?\d+)@@")


def open_serial(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    speed = getattr(termios, f"B{baud}")
    attrs[0] = 0                                  # iflag
    attrs[1] = 0                                  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                  # lflag: raw
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def drain(fd, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r and not os.read(fd, 4096):
            break


def flash(openocd_cfg, elf, halt):
    # Halting after programming lets the serial reader start before the
    # firmware prints anything; the reset is issued separately.
    post = "reset halt" if halt else "reset run"
    cmd = ["openocd", "-f", openocd_cfg,
           "-c", f"program {elf} verify; {post}; shutdown"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode(errors="replace")


def reset_run(openocd_cfg):
    cmd = ["openocd", "-f", openocd_cfg, "-c", "init; reset run; shutdown"]
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).returncode


def run_one(args, elf):
    rc, log = flash(args.openocd_cfg, elf, halt=True)
    if rc != 0:
        return None, b"", "flash failed:\n" + log[-2000:]

    fd = open_serial(args.port, args.baud)
    try:
        drain(fd, 0.1)
        if reset_run(args.openocd_cfg) != 0:
            return None, b"", "reset failed"
        buf = b""
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                chunk = os.read(fd, 4096)
                buf += chunk
                if args.verbose:
                    sys.stdout.write(chunk.decode(errors="replace"))
                    sys.stdout.flush()
                m = EXIT_RE.search(buf)
                if m:
                    return int(m.group(1)), buf, None
        return None, buf, f"timeout after {args.timeout}s"
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elfs", nargs="+")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--openocd-cfg", default="board/stm32f769i-disco.cfg")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--log-dir", help="write each test's console output here")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.log_dir:
        os.makedirs(args.log_dir, exist_ok=True)

    results = []
    for elf in args.elfs:
        name = os.path.splitext(os.path.basename(elf))[0]
        print(f"[RUN ] {name}", flush=True)
        start = time.monotonic()
        code, out, err = run_one(args, elf)
        elapsed = time.monotonic() - start
        if args.log_dir:
            with open(os.path.join(args.log_dir, name + ".log"), "wb") as f:
                f.write(out)
        ok = code == 0
        status = "PASS" if ok else "FAIL"
        detail = f"exit={code}" if err is None else err
        print(f"[{status}] {name} ({detail}, {elapsed:.1f}s)", flush=True)
        if not ok and not args.verbose:
            tail = out.decode(errors="replace").strip().splitlines()[-15:]
            for line in tail:
                print(f"    | {line}")
        results.append((name, ok, detail))

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    for name, _, detail in failed:
        print(f"  FAILED: {name} ({detail})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
