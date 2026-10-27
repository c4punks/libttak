# libttak on the STM32F769I-DISCO over Ethernet

This is a network service running on real hardware: FreeRTOS, lwIP, the ST HAL Ethernet driver and the
board's LAN8742A PHY. Everything above the netif is libttak (`PLATFORM=freertos NET=lwip`):

```
 host ──TCP──► lwIP sockets ──► ttak_net_poller (one task, all sockets)
                                   │ readable
                                   ▼
                     ttak_net_view_readv() ──► ttak_thread_pool task (upper-case)
                                   │ future
                                   ▼
                     ttak_io_sync_write() ──► lwIP ──► ETH DMA ──► LAN8742A ──► wire
```

- **Port 7000 (echo):** every chunk a client sends is upper-cased by a libttak thread-pool task and sent back.
- **Port 7001 (stats):** replies with one line of counters, then closes:
  - libttak: `ttak_mem_usage_bytes()` and `ttak_mem_buddy_get_stats()`.
  - Heaps: newlib `mallinfo()` and the FreeRTOS heap.
  - Service: connections, pool tasks and bytes handled.

## Results

All runs used the same STM32F769I-DISCO (STM32F769NI, 216 MHz). The board was on a 100 Mbit/s home LAN,
and the host was a Linux PC on the same switch. Each run reflashed the board, rebooted it, and ran
`scripts/baremetal/stm32_eth_demo.py`. **5 of 5 runs passed all 7 checks.** This table is from the first:

| Test | Result | Detail |
|---|---|---|
| boot | pass | DHCP address after 15.5 s, flashing included (13.5 to 16.1 s over the 5 runs) |
| ping | pass | 20 pings, 0% loss, rtt min/avg/max 0.11/0.12/0.22 ms |
| echo | pass | 500 chunks of 1 to 1460 bytes upper-cased by the thread pool, all verified |
| throughput | pass | 4 MiB echoed in 1.73 s = **19.4 Mbit/s each way** (19.4 to 19.5 over the 5 runs) |
| parallel | pass | 4 connections x 256 KiB in 0.63 s (0.61 to 0.68) |
| churn | pass | 200 connect/send/close cycles in 0.2 s |
| memory | pass | ttak_mem, buddy, newlib heap and FreeRTOS heap all identical before and after 205 connections and 3793 pool tasks |

Throughput is limited by the design, not by the link. One task serves every client, and each chunk is a
pool round trip: submit, then wait on a future. lwIP and the MAC are not the bottleneck.

Memory, from `arm-none-eabi-size` and the link map:

| Region | Use |
|---|---|
| Flash | 133 KiB code, constants and initialised data |
| SRAM1+2 (384 KiB) | 316 KiB `.bss`: libttak's static pools, lwIP heap and pools, 32 RX DMA buffers (48 KiB). The rest is newlib heap and the main stack. |
| DTCM (128 KiB) | 112 KiB FreeRTOS heap (task stacks, kernel objects). The minimum ever free was 63 KiB. |

## Build and run

You need the arm-none-eabi toolchain, OpenOCD, rsync, the board on `/dev/ttyACM0`, and an Ethernet cable to a
LAN with a DHCP server. Fetch the third-party sources first:

```sh
git clone --depth 1 --branch V11.1.0 https://github.com/FreeRTOS/FreeRTOS-Kernel
git clone --depth 1 --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip
git clone --depth 1 https://github.com/STMicroelectronics/stm32f7xx-hal-driver   # HAL 1.3.3
git clone --depth 1 https://github.com/STMicroelectronics/cmsis-device-f7
git clone --depth 1 https://github.com/STMicroelectronics/cmsis-core
git clone --depth 1 https://github.com/STMicroelectronics/stm32-lan8742
```

These are also inside an STM32CubeF7 package:
- `Drivers/STM32F7xx_HAL_Driver`
- `Drivers/CMSIS/Device/ST/STM32F7xx`
- `Drivers/CMSIS`
- `Drivers/BSP/Components/lan8742`

Then build and run:

```sh
make FREERTOS_KERNEL_PATH=... LWIP_PATH=... STM32_HAL_PATH=... \
     STM32_CMSIS_DEVICE_PATH=... CMSIS_CORE_PATH=... LAN8742_PATH=...
../../scripts/baremetal/stm32_eth_demo.py build/stm32f769i_disco_eth.elf
```

The Makefile builds its own copy of libttak under `build/libttak`, compiled against this directory's
`FreeRTOSConfig.h` and `lwipopts.h`, so the checkout's `lib/` is left alone. The console (115200 8N1 on
the ST-LINK virtual COM port) looks like this:

```
[app] libttak on STM32F769I-DISCO, 216 MHz, FreeRTOS + lwIP
[eth] MAC 02:54:2f:36:2e:be, PHY at MDIO address 0
[eth] link up, 100 Mbit/s full duplex
[net] address 192.168.0.219, gateway 192.168.0.1
@@ETH_READY ip=192.168.0.219@@
[app] echo on 7000, stats on 7001 (libttak poller + 2 pool workers)
```

## What it took

Each of these broke the bring-up, and each fix is in this directory or in libttak.

1. **About half the boots received nothing (RMII receive phase).** Every frame was a CRC error.
   - Dumping the frames showed bits 0 and 4 of every byte set: RXD0 read as 1 on every other `REF_CLK`
     cycle.
   - It is decided at reset, and it did not depend on the clock source (HSE or HSI) or on the init order.
   - ST describes the same behaviour as an RMII timing issue between the MAC's RMII clock and its half-rate
     copy (STM32MP25 erratum ES0598 2.22.2).
   - Restarting the MAC with its clocks gated draws a new phase. The link task watches the MMC CRC counter
     and restarts the MAC when errors pile up with no good frame. After 3 tries it resets the chip.
   - The MAC needed 0 or 1 restarts over the 5 runs above; a sixth run needed 2 and also passed.
2. **ICMP replies left with checksum 0.** `HAL_ETH_Init()` presets every TX descriptor to full
   hardware checksum insertion, and the preset only changes when the TX config asks for checksums. With
   lwIP computing checksums too, the MAC "re-computed" an already valid ICMP checksum to 0. The example
   uses hardware checksums both ways and turns lwIP's off (`lwipopts.h`).
3. **The MAC restart wedged the stack.** `HAL_ETH_Init()` resets the descriptors but not the handle's
   bookkeeping, so after a restart every transmit returned BUSY. The link task sat in it while holding
   the tcpip core lock. `mac_restart()` now releases every buffer and clears that state. The transmit wait
   is bounded, and the input task and link task serialise on a mutex.
4. **The same DHCP xid on every boot.** lwIP uses `rand()` for DHCP xids, TCP sequence numbers and ports.
   Without a seed it repeats them every boot, and a DHCP server may ignore a Discover it has already
   answered. `main.c` seeds it from the hardware RNG.
5. **Parallel clients deadlocked.** RX is zero-copy, so unread data keeps its DMA buffer. With 12 buffers,
   idle connections' unread data starved the ACKs a blocked writer was waiting for. The buffer count now
   covers every client's TCP window (6 x 4 segments, plus 8 spare).
6. **libttak's sync I/O leaked; fixed in libttak.**
   - `ttak_io_sync_read()`/`_write()` stage data in a detachable buffer.
   - The default detachable context retires frees to the epoch, and nothing reclaims them, so every call
     leaked its buffer. That came to 42 MB per 20000 hosted round trips; this board ran out of heap within
     a few hundred.
   - I/O buffers now come from a context that frees at once.
   - Separately, on bare metal the detachable quarantine was capped at 1 MiB, more than the whole heap.
     It is now 8 KiB on bare metal (`TTAK_DETACHABLE_QUARANTINE_BYTE_LIMIT`).
   - `test_net_tcp` checks both.
7. **Smaller fixes:**
   - `dhcp_start()` refuses a netif that is administratively down, so the netif goes up before the cable
     state is known.
   - The poller allocates per call, so the service backs off on errors instead of spinning.

## Limits

- The D-cache is off, so DMA buffers need no cache maintenance. Turning it on needs an MPU region for the
  descriptors and buffers, or cache clean/invalidate calls in `ethernetif.c`.
- One task serves all clients, and writes block. A slow reader holds up the others until its TCP window
  drains.
- `ttak_net_poller_wait()` mallocs its `pollfd` array on every call.
