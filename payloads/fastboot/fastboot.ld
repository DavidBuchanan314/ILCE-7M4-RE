/*
 * eSRAM regions already spoken for:
 *   0xFE005000..0xFE005080  ROM scratch (later the Linux spin-table)
 *   0xFE005800              ROM trace buffer
 *   0xFE010010              decoded straps cached by the ROM
 *   0xFE030008              16 bytes of ROM eMMC device state
 *   0xFE040000+             eMMC loader staging on a normal boot
 *
 * The serial-boot protocol stages payloads at 0xFE020000
 * (scripts/push_payload.py:LOAD_ADDR).
 *
 * The DMA arena is a fixed region rather than .bss so the DWC3's AXI master
 * has a permanently-valid address range, and starts at 0xFE100000 to clear the
 * ROM's eMMC state.
 */

ENTRY(_start)

MEMORY
{
    ESRAM (rwx) : ORIGIN = 0xFE020000, LENGTH = 64K
    USBDMA (rw) : ORIGIN = 0xFE100000, LENGTH = 1M
}

STACK_SIZE = 8K;

SECTIONS
{
    .text : {
        KEEP(*(.text.start))     /* _start must land at 0xFE020000 */
        *(.text .text.*)
    } > ESRAM

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    } > ESRAM

    .data : ALIGN(8) {
        *(.data .data.*)
    } > ESRAM

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > ESRAM

    .stack (NOLOAD) : ALIGN(16) {
        . += STACK_SIZE;
        __stack_top = .;
    } > ESRAM

    /* Separate region, so a .text overrun is a link error. */
    .usbdma (NOLOAD) : ALIGN(64) {
        __usbdma_start = .;
        *(.usbdma .usbdma.*)
        . = ALIGN(64);
        __usbdma_end = .;
    } > USBDMA


    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.ARM.attributes) }
}
