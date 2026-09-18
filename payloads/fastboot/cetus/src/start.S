/*
 * Entered by the mask ROM's Entry command: the monitor stores this address to
 * the hand-off vector at 0xFE00500C and returns, and the ROM's outer loop
 * branches here. We arrive at EL3 with the MMU off and no usable stack.
 */

    .arch armv8-a

    .section .text.start, "ax"
    .global _start
_start:
    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b

2:  bl      main

3:  wfe
    b       3b

    .ltorg
