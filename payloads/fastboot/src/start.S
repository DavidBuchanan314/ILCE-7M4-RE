/*
 * Entry point. The bootrom serial-boot protocol stages this image at
 * 0xFE020000 and jumps to it via a type-0xFFFF record, so we arrive with:
 *   - AArch64, EL3 (the whole boot chain on Mira stays at EL3)
 *   - MMU off, so all data accesses are Device-nGnRnE: uncached, strongly
 *     ordered. main() verifies this rather than trusting it.
 *   - no stack worth the name
 *
 * We do not touch SCTLR, the MMU or the caches. Leaving the MMU off is what
 * makes DMA to the event buffer and TRB rings coherent for free.
 */

    .arch armv8-a

    .section .text.start, "ax"
    .global _start
_start:
    /* Park every core but the primary. The ROM should only be running one,
     * but a stray secondary scribbling on our stack would be invisible. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbz     x0, 1f
0:  wfe
    b       0b

1:  ldr     x0, =__stack_top
    mov     sp, x0

    /* Zero .bss. */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:  cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b

3:  bl      main

    /* main() is not expected to return. If it does, blink forever so the
     * failure is at least visible. */
    bl      led_panic
4:  b       4b

    .ltorg
