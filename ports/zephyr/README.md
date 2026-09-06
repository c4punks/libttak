# Zephyr port

A from-scratch buddy allocator backend for Zephyr's `lib/heap`, written in
Zephyr's own idiom (`struct z_buddy_heap`, `k_spinlock`) rather than a literal
copy of `src/phys/mem/buddy.c`'s `ttak_mem_*` API. It exists here so the
Zephyr-side integration has a proper source of truth instead of living only
as untracked files inside a Zephyr checkout.

To install into a Zephyr tree:

```
cp z_buddy.h  <zephyr>/include/zephyr/sys/z_buddy.h
cp z_buddy.c  <zephyr>/lib/heap/z_buddy.c
```

`<zephyr>/include/zephyr/sys/sys_heap.h`, `<zephyr>/lib/heap/Kconfig`,
`<zephyr>/lib/heap/CMakeLists.txt`, and `<zephyr>/lib/heap/heap.c` also carry
local modifications (in that Zephyr checkout, not tracked here) to route heap
calls to this backend when selected.

## Fixed in this port

`z_buddy.h` originally pulled in `struct k_spinlock` only transitively via
`<zephyr/kernel.h>`. That header is reachable from inside `kernel.h`'s own
include chain (`kernel.h` -> `kernel_includes.h` -> `kernel_structs.h` ->
`sys_heap.h` -> here), so on that path `kernel.h`'s include guard is already
active by the time this header is processed, the transitive include becomes
a no-op, and `struct k_spinlock` has not been defined yet — every build
failed with `field 'lock' has incomplete type`, for every board and sample,
not just display-related ones. Fixed by including `<zephyr/spinlock.h>`
directly instead of relying on include order.
