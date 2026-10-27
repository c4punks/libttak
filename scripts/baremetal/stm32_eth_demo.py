#!/usr/bin/env python3
"""Flash the STM32F769I-DISCO Ethernet example and test it over the network.

    scripts/baremetal/stm32_eth_demo.py examples/stm32f769i-disco-lwip/build/stm32f769i_disco_eth.elf

The board must be cabled to the same LAN as this machine, with a DHCP
server. The script flashes the image, reads the console until the board
prints "@@ETH_READY ip=...@@", then checks:

  ping         ICMP echo, loss and round-trip time
  echo         500 random-length chunks come back upper-cased (thread pool)
  throughput   one connection streaming through the echo service
  parallel     4 connections at once
  churn        200 connect/send/close cycles
  memory       after the throughput, parallel and churn tests, libttak, newlib
               and FreeRTOS heap usage are back where they were after echo

and prints a Markdown summary. Pass --ip to skip flashing and test a board
that is already running.
"""

import argparse
import importlib.util
import os
import random
import re
import select
import socket
import string
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
READY_RE = re.compile(rb"@@ETH_READY ip=([0-9.]+)@@")
EXIT_RE = re.compile(rb"@@TTAK_EXIT=(-?\d+)@@")
ECHO_PORT = 7000
STATS_PORT = 7001


def load_runner():
    sys.dont_write_bytecode = True  # no __pycache__ next to the scripts
    spec = importlib.util.spec_from_file_location("run_stm32", os.path.join(HERE, "run_stm32.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Console:
    """Keeps reading the board's console in the background."""

    def __init__(self, fd, text=b""):
        self.fd, self.text = fd, text
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    self.text += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass

    def tail(self, lines=15):
        return "\n".join(self.text.decode(errors="replace").splitlines()[-lines:])


def boot(elf, port, cfg, timeout):
    """Flash, reset, and return (ip, console log, serial fd)."""
    runner = load_runner()
    fd = runner.open_serial(port, 115200)
    rc, out = runner.flash(cfg, elf, True)
    if rc != 0:
        sys.exit(f"flashing failed:\n{out}")
    runner.drain(fd, 0.3)
    runner.reset_run(cfg)
    log = b""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            log += os.read(fd, 4096)
        except BlockingIOError:
            continue
        m = READY_RE.search(log)
        if m:
            return m.group(1).decode(), log, fd
        if EXIT_RE.search(log):
            break
    sys.exit("board did not come up:\n" + log.decode(errors="replace"))


def stats(ip):
    with socket.create_connection((ip, STATS_PORT), timeout=5) as s:
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(512)
            if not chunk:
                break
            data += chunk
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", data.decode())}


def ping(ip, count=20):
    out = subprocess.run(["ping", "-c", str(count), "-i", "0.2", "-W", "1", ip],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode()
    loss = re.search(r"([\d.]+)% packet loss", out)
    rtt = re.search(r"= ([\d.]+)/([\d.]+)/([\d.]+)", out)
    return (float(loss.group(1)) if loss else 100.0,
            tuple(float(x) for x in rtt.groups()) if rtt else None)


def random_lower(n):
    return "".join(random.choice(string.ascii_lowercase + " ") for _ in range(n)).encode()


def echo_stream(ip, payload, chunk=1460):
    """Send payload and read the reply concurrently; return (reply, seconds)."""
    s = socket.create_connection((ip, ECHO_PORT), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    got = bytearray()

    def reader():
        while len(got) < len(payload):
            data = s.recv(65536)
            if not data:
                break
            got.extend(data)

    t = threading.Thread(target=reader)
    start = time.monotonic()
    t.start()
    for i in range(0, len(payload), chunk):
        s.sendall(payload[i:i + chunk])
    t.join(60)
    elapsed = time.monotonic() - start
    s.close()
    return bytes(got), elapsed


def test_echo(ip, rounds=500):
    try:
        return _test_echo(ip, rounds)
    except OSError:
        return False


def _test_echo(ip, rounds):
    with socket.create_connection((ip, ECHO_PORT), timeout=10) as s:
        for _ in range(rounds):
            msg = random_lower(random.randint(1, 1460))
            s.sendall(msg)
            got = b""
            while len(got) < len(msg):
                data = s.recv(4096)
                if not data:
                    return False
                got += data
            if got != msg.upper():
                return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?")
    ap.add_argument("--ip", help="test a board that is already running")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--openocd-cfg", default="board/stm32f769i-disco.cfg")
    ap.add_argument("--boot-timeout", type=float, default=90)
    ap.add_argument("--bytes", type=int, default=4 << 20, help="throughput payload size")
    args = ap.parse_args()
    if not args.ip and not args.elf:
        ap.error("give an ELF to flash or --ip")

    random.seed(1)
    results = []
    failed = False

    def record(name, ok, detail):
        nonlocal failed
        failed |= not ok
        results.append((name, "pass" if ok else "FAIL", detail))
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
        if not ok and console:
            print("--- board console ---\n" + console.tail() + "\n---", flush=True)

    console = None
    if args.ip:
        ip = args.ip
    else:
        t0 = time.monotonic()
        ip, log, fd = boot(args.elf, args.port, args.openocd_cfg, args.boot_timeout)
        console = Console(fd, log)
        restarts = len(re.findall(rb"restarting the MAC", log))
        record("boot", True, f"DHCP address {ip} after {time.monotonic() - t0:.1f} s "
                             f"(flashing included), {restarts} MAC restart(s)")

    loss, rtt = ping(ip)
    record("ping", loss == 0.0,
           f"{loss:.0f}% loss" + (f", rtt min/avg/max {rtt[0]:.2f}/{rtt[1]:.2f}/{rtt[2]:.2f} ms" if rtt else ""))

    record("echo", test_echo(ip), "500 chunks of 1-1460 bytes upper-cased by the thread pool")
    # Baseline once every code path has run: first use allocates for good
    # (thread-pool workers, owner tables), which is not a leak.
    time.sleep(0.5)
    base = stats(ip)

    payload = random_lower(args.bytes)
    got, secs = echo_stream(ip, payload)
    mbit = len(payload) * 8 / secs / 1e6
    record("throughput", got == payload.upper(),
           f"{len(payload) >> 20} MiB echoed in {secs:.2f} s = {mbit:.1f} Mbit/s each way")

    par = [None] * 4

    def run(i):
        data = random_lower(256 << 10)
        par[i] = echo_stream(ip, data)[0] == data.upper()

    ts = [threading.Thread(target=run, args=(i,)) for i in range(4)]
    t0 = time.monotonic()
    for t in ts:
        t.start()
    for t in ts:
        t.join(120)
    record("parallel", all(par), f"4 connections x 256 KiB in {time.monotonic() - t0:.2f} s")

    t0 = time.monotonic()
    ok = True
    for _ in range(200):
        try:
            with socket.create_connection((ip, ECHO_PORT), timeout=5) as s:
                s.sendall(b"churn")
                ok &= s.recv(16) == b"CHURN"
        except OSError:
            ok = False
    record("churn", ok, f"200 connect/send/close cycles in {time.monotonic() - t0:.1f} s")

    time.sleep(0.5)  # let the last connections close on the board
    after = stats(ip)
    keys = ("ttak_mem", "buddy_in_use", "libc_heap", "rtos_heap_free")
    mem_ok = all(after[k] == base[k] for k in keys)
    record("memory", mem_ok,
           f"ttak_mem {base['ttak_mem']} -> {after['ttak_mem']} B, "
           f"buddy {base['buddy_in_use']} -> {after['buddy_in_use']} B, "
           f"newlib heap {base['libc_heap']} -> {after['libc_heap']} B, "
           f"FreeRTOS heap free {base['rtos_heap_free']} -> {after['rtos_heap_free']} B "
           f"(min ever {after['rtos_heap_min']} B) across "
           f"{after['accepted'] - base['accepted']} connections and {after['tasks'] - base['tasks']} pool tasks")

    print("\n| Test | Result | Detail |\n|---|---|---|")
    for name, res, detail in results:
        print(f"| {name} | {res} | {detail} |")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
