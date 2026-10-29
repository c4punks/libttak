#!/usr/bin/env python3
"""Check -fstack-usage output against per-architecture byte thresholds.

Parses GCC .su files ("file:line:col:function<TAB>frame_size<TAB>type")
produced by bare-metal builds (see STACK_USAGE=1 in the Makefile) and
fails when any function's stack frame exceeds the configured threshold
for the target core.

This is a per-function bound, not a full static call-graph analysis
(that needs link-time information): as a pragmatic recursion proxy the
script also prints the top-N largest frames and the total of all frames.

Usage:
  check_stack_usage.py --core cortex-m7 [--obj-dir obj]
                       [--config scripts/baremetal/stack_usage_thresholds.json]
                       [--threshold BYTES] [--top 20]
"""

import argparse
import json
import re
import sys
from pathlib import Path

SU_LINE = re.compile(r"^(?P<loc>\S+?)(?::\d+){0,2}:(?P<func>[^\t]+)\t(?P<size>\d+)\t(?P<kind>\S+)")


def parse_su(path: Path):
    for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        m = SU_LINE.match(line)
        if not m:
            print(f"warning: {path}:{lineno}: unparseable line: {line!r}", file=sys.stderr)
            continue
        yield {
            "func": m.group("func"),
            "size": int(m.group("size")),
            "loc": m.group("loc"),
            "kind": m.group("kind"),
            "file": str(path),
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--core", required=True,
                    help="target core name, e.g. cortex-m7 (key into the config)")
    ap.add_argument("--obj-dir", default="obj",
                    help="directory tree to scan for .su files (default: obj)")
    ap.add_argument("--config", default=str(Path(__file__).with_name("stack_usage_thresholds.json")),
                    help="JSON file with per-core thresholds")
    ap.add_argument("--threshold", type=int, default=None,
                    help="override the configured threshold (bytes)")
    ap.add_argument("--top", type=int, default=20,
                    help="how many largest frames to print (default: 20)")
    args = ap.parse_args()

    if args.threshold is None:
        cfg = json.loads(Path(args.config).read_text())
        try:
            args.threshold = int(cfg["thresholds"][args.core])
        except KeyError:
            sys.exit(f"error: no threshold for core '{args.core}' in {args.config}")

    su_files = sorted(Path(args.obj_dir).rglob("*.su"))
    if not su_files:
        sys.exit(f"error: no .su files under {args.obj_dir}; "
                 "build with PLATFORM=baremetal (STACK_USAGE=1) first")

    frames = [f for p in su_files for f in parse_su(p)]
    frames.sort(key=lambda f: f["size"], reverse=True)
    total = sum(f["size"] for f in frames)

    print(f"[stack-usage] core={args.core} threshold={args.threshold} B "
          f"functions={len(frames)} su_files={len(su_files)} total={total} B")

    print(f"[stack-usage] top {min(args.top, len(frames))} largest frames:")
    for f in frames[:args.top]:
        print(f"  {f['size']:>8}  {f['func']}  ({f['loc']})")

    offenders = [f for f in frames if f["size"] > args.threshold]
    if offenders:
        print(f"\n[stack-usage] FAIL: {len(offenders)} function(s) exceed "
              f"{args.threshold} B on {args.core}:", file=sys.stderr)
        for f in offenders:
            print(f"  {f['size']:>8}  {f['func']}  ({f['loc']})", file=sys.stderr)
        sys.exit(1)

    print(f"[stack-usage] OK: all frames <= {args.threshold} B")


if __name__ == "__main__":
    main()
