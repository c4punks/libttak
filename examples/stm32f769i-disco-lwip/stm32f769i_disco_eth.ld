/* STM32F769NI for the Ethernet example.
 * RAM is SRAM1+SRAM2 (384 KiB), which the Ethernet DMA can reach: data,
 * bss (lwIP, libttak pools, DMA descriptors and buffers), newlib heap
 * and the main stack. DTCM (128 KiB, CPU only) holds the FreeRTOS heap. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
    DTCM  (rw)  : ORIGIN = 0x20000000, LENGTH = 128K
    RAM   (rwx) : ORIGIN = 0x20020000, LENGTH = 384K
}

SECTIONS
{
    .dtcm (NOLOAD) : ALIGN(8)
    {
        *(.dtcm .dtcm.*)
    } > DTCM
}

/* Shared with the bare-metal test harness (tests/baremetal). */
INCLUDE sections_cortex_m.ld
