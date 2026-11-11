ENTRY(_start)

SECTIONS
{
    . = 0x80000000; /* RAM base: with -bios none, QEMU jumps here after reset */

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .eh_frame : { KEEP(*(.eh_frame)) } /* Unwind info, used by debuggers for backtraces */

    .data : {
        *(.data .data.*)
        /* gp points 2KB into the small data, so the 12-bit signed offsets of gp-relative
           accesses can reach 4KB of .sdata/.sbss around it */
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
    }

    /* Zeroed by startup.S */
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 0x4000; /* Allocate 16KB stack space */
    _stack_top = .;
}
