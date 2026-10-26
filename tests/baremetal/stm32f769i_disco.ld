/* STM32F769NI: 2 MiB flash; DTCM + SRAM1 + SRAM2 form 512 KiB contiguous RAM. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

INCLUDE sections_cortex_m.ld
