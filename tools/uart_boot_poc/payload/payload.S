    .arch armv8-a

    .equ ROM_LED_INIT, 0xffff0784
    .equ LED_BASE,     0xf102f044
    .equ LED_SET,      0x4

    .text
    .global _start
_start:
    ldr  w0, =ROM_LED_INIT
    blr  x0
    ldr  w8, =LED_BASE
    mov  w9, #1
    str  w9, [x8, #LED_SET]
spin:
    b    spin

    .ltorg
