/* QEMU mps2-an500: SSRAM1 at 0x0 holds code, SSRAM2/3 at 0x20000000 is data.
 * RAM is capped at 512 KiB to match the STM32F769 budget. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4096K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

INCLUDE sections_cortex_m.ld
