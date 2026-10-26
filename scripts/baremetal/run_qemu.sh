#!/bin/sh
# Run bare-metal unit-test images on QEMU's mps2-an500 (Cortex-M7).
#
#   make PLATFORM=baremetal BOARD=mps2_an500 baremetal-tests
#   scripts/baremetal/run_qemu.sh tests/baremetal/build/mps2_an500/*.elf
#
# Output and the exit code come back over semihosting.

QEMU="${QEMU:-qemu-system-arm}"
TIMEOUT="${TIMEOUT:-120}"

if [ "$#" -eq 0 ]; then
    echo "usage: $0 test.elf..." >&2
    exit 2
fi

pass=0
fail=0
failed=""
for elf in "$@"; do
    name=$(basename "$elf" .elf)
    echo "[RUN ] $name"
    timeout "$TIMEOUT" "$QEMU" -M mps2-an500 -nographic -monitor none -serial none \
        -semihosting-config enable=on,target=native -kernel "$elf"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "[PASS] $name"
        pass=$((pass + 1))
    else
        [ "$rc" -eq 124 ] && echo "[FAIL] $name (timeout after ${TIMEOUT}s)" \
                          || echo "[FAIL] $name (exit=$rc)"
        fail=$((fail + 1))
        failed="$failed $name"
    fi
done

echo
echo "$pass/$((pass + fail)) passed"
[ -n "$failed" ] && echo "  FAILED:$failed"
[ "$fail" -eq 0 ]
