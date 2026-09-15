/*
 * Link map for the bootrom-staged fastboot payload.
 *
 * eSRAM spans 0xFE000000..0xFEFFFFFF. Regions already spoken for:
 *   0xFE005000..0xFE005080  ROM scratch (and later the Linux spin-table)
 *   0xFE005800              ROM trace buffer
 *   0xFE010010              decoded straps cached by the ROM
 *   0xFE040000+             where the eMMC loader gets staged on a normal boot
 *
 * That leaves 0xFE020000..0xFE040000 free, which is why the serial-boot
 * protocol stages payloads at 0xFE020000 (scripts/push_payload.py:LOAD_ADDR).
 *
 * We split that 128 KiB window in half:
 *   0xFE020000..0xFE030000  code, rodata, data, bss, stack
 *   0xFE100000..0xFE200000  USB DMA arena (event buffer, TRBs, download buffer)
 *
 * The arena deliberately does NOT start at 0xFE030000. The mask ROM keeps its
 * eMMC device state in 16 bytes at 0xFE030008 (written by 0xFFFF3188, read
 * back by the flash reader at 0xFFFF2DDC), which landed squarely inside our
 * 4 KB USB event buffer. Any call into the ROM's eMMC code would have
 * corrupted USB events in flight.
 *
 * The arena runs past the 0xFE040000 mark that the eMMC loader would normally
 * be staged at. That is fine here: this payload replaced the loader, so
 * nothing else is live in eSRAM above the ROM's own scratch (0xFE005000
 * .. 0xFE005800, trace at 0xFE005800, cached straps at 0xFE010010).
 *
 * The DMA arena is carved out at a fixed address rather than allocated from
 * .bss so the DWC3's AXI master sees a known, permanently-valid region, and so
 * the addresses stay stable across builds while we are debugging blind.
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

    /*
     * DMA arena. NOLOAD: nothing here is initialised from the image, it is
     * zeroed at runtime by usb_dma_init(). Placed in its own region so a
     * .text overrun shows up as a link error rather than as silent corruption
     * of the TRB rings.
     */
    .usbdma (NOLOAD) : ALIGN(64) {
        __usbdma_start = .;
        *(.usbdma .usbdma.*)
        . = ALIGN(64);
        __usbdma_end = .;
    } > USBDMA


    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.ARM.attributes) }
}
