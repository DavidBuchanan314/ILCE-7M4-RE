/*
 * Link map for a payload staged into Cetus eSRAM over the mask ROM's SPI
 * monitor and entered with its Entry command.
 *
 * Regions the ROM itself owns:
 *   0xFE005000..0xFE005080  ROM scratch; 0xFE00500C is the hand-off vector
 *   0xFE005800              ROM trace buffer
 *   0xFE010010              monitor command/response frame
 *   0xFE010030..0xFE010090  ROM driver config, including the OSPI state
 *   0xFE010070              monitor work buffer
 *   0xFE014AA4..0xFE014AB8  monitor sequence counters
 *   0xFE040000+             where a NOR boot stages the EXBL image
 *
 * Entering the monitor does not clear eSRAM, so everything below survives a
 * `cetus boot` -- which is the whole basis of the mailbox: the payload halts,
 * the AP resets the CP to get the monitor back, and then reads the results.
 */

ENTRY(_start)

MEMORY
{
    CODE (rwx) : ORIGIN = 0xFE020000, LENGTH = 64K
    MBOX (rw)  : ORIGIN = 0xFE030000, LENGTH = 4K
}

STACK_SIZE = 4K;

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > CODE

    .rodata : ALIGN(8) { *(.rodata .rodata.*) } > CODE
    .data   : ALIGN(8) { *(.data .data.*) }     > CODE

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > CODE

    .stack (NOLOAD) : ALIGN(16) {
        . += STACK_SIZE;
        __stack_top = .;
    } > CODE

    /* Fixed address so the AP can find it with no symbol table. */
    .mbox (NOLOAD) : ALIGN(64) { KEEP(*(.mbox)) } > MBOX

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.ARM.attributes) }
}
