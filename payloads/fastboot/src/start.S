/*
 * Entered from the bootrom's serial-boot protocol via a type-0xFFFF record,
 * at EL3 with the MMU off and no stack. The MMU stays off, which is what keeps
 * DMA to the event buffer and TRB rings coherent.
 */

    .arch armv8-a

    .section .text.start, "ax"
    .global _start
_start:
    /* Park every core but the primary. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbz     x0, 1f
0:  wfe
    b       0b

1:  ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:  cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b

3:  bl      main

    bl      led_panic
4:  b       4b

    .ltorg
