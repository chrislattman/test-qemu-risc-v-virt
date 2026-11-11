.section ".text.boot", "ax"
.global _start

// QEMU starts every hart here in machine mode (M-mode), with a0 = hart ID and a1 = device tree
_start:
    .cfi_startproc
    .cfi_undefined ra           // Outermost frame for debuggers
    // Force harts other than hart 0 into an infinite sleep loop
    csrr    t0, mhartid
    beqz    t0, master_hart
hang:
    wfi
    j       hang

master_hart:
    // Set up the global pointer, which the linker uses to shorten accesses to small data
    // (relaxation must be off here, or `la gp` would be relaxed into a gp-relative load of itself)
.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    // Set the trap vector to trap_entry (direct mode: every trap goes to the same address)
    la      t0, trap_entry
    csrw    mtvec, t0

    // Set up a temporary safe stack pointer region inside RAM
    la      sp, _stack_top

    // Zero .bss, which C requires for zero-initialized statics
    la      t0, __bss_start
    la      t1, __bss_end
zero_bss:
    bgeu    t0, t1, zero_bss_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       zero_bss
zero_bss_done:

    // Jump straight to main architecture loop (interrupts stay off until main_app enables them)
    call    main_app

    // Defensive fallback hang loop
    j       hang
    .cfi_endproc

// --- Trap entry ---
// mtvec requires 4-byte alignment in direct mode. On a trap, the hart saves the PC in mepc and the
// cause in mcause, clears mstatus.MIE (masking interrupts), and jumps here
.align 2
trap_entry:
    .cfi_startproc
    .cfi_undefined ra           // Outermost frame for debuggers: return address is in mepc, not ra
    // Context Switch Save Pass: Push caller-saved registers onto the stack (callee-saved registers
    // are preserved by handle_trap itself). RV64IMAC has no FP registers to save
    addi    sp, sp, -128
    sd      ra,   0(sp)
    sd      t0,   8(sp)
    sd      t1,  16(sp)
    sd      t2,  24(sp)
    sd      a0,  32(sp)
    sd      a1,  40(sp)
    sd      a2,  48(sp)
    sd      a3,  56(sp)
    sd      a4,  64(sp)
    sd      a5,  72(sp)
    sd      a6,  80(sp)
    sd      a7,  88(sp)
    sd      t3,  96(sp)
    sd      t4, 104(sp)
    sd      t5, 112(sp)
    sd      t6, 120(sp)

    csrr    a0, mcause          // Pass the trap cause as handle_trap's argument
    call    handle_trap         // Run our C function line

    // Context Switch Restore Pass: Pop registers cleanly off stack memory
    ld      ra,   0(sp)
    ld      t0,   8(sp)
    ld      t1,  16(sp)
    ld      t2,  24(sp)
    ld      a0,  32(sp)
    ld      a1,  40(sp)
    ld      a2,  48(sp)
    ld      a3,  56(sp)
    ld      a4,  64(sp)
    ld      a5,  72(sp)
    ld      a6,  80(sp)
    ld      a7,  88(sp)
    ld      t3,  96(sp)
    ld      t4, 104(sp)
    ld      t5, 112(sp)
    ld      t6, 120(sp)
    addi    sp, sp, 128
    mret                        // Machine-mode trap return back to your super loop (restores MIE)
    .cfi_endproc
