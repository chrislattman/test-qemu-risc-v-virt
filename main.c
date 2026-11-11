#include <stdint.h>

// --- UART NS16550A ---
#define UART0_BASE               0x10000000
#define UART0_THR                ((volatile uint8_t *)(UART0_BASE + 0x00)) // Transmit Holding Register
#define UART0_IER                ((volatile uint8_t *)(UART0_BASE + 0x01)) // Interrupt Enable Register
#define UART0_LSR                ((volatile uint8_t *)(UART0_BASE + 0x05)) // Line Status Register
#define UART_IER_ETBEI           (1 << 1)                                  // THR empty interrupt enable
#define UART_LSR_THRE            (1 << 5)                                  // THR empty
#define UART0_IRQ                10                                        // UART0's PLIC source ID

// --- PLIC (QEMU virt Memory Map) ---
// The Platform-Level Interrupt Controller routes device ("external") interrupts to harts, like the
// GIC distributor + CPU interface. Context 0 is hart 0 in machine mode (context 1 is its S-mode)
#define PLIC_BASE        0x0C000000
#define PLIC_PRIORITY(id) ((volatile uint32_t *)(PLIC_BASE + 0x000000 + 4 * (id)))  // Per-source priority
#define PLIC_ENABLE0     ((volatile uint32_t *)(PLIC_BASE + 0x002000))  // Context 0 enables, sources 0-31
#define PLIC_THRESHOLD0  ((volatile uint32_t *)(PLIC_BASE + 0x200000))  // Context 0 priority threshold
#define PLIC_CLAIM0      ((volatile uint32_t *)(PLIC_BASE + 0x200004))  // Context 0 claim/complete

// --- CLINT (QEMU virt Memory Map) ---
// The Core-Local Interruptor provides each hart's timer and machine software interrupt
#define CLINT_BASE       0x02000000
#define CLINT_MSIP       ((volatile uint32_t *)(CLINT_BASE + 0x0000)) // Hart 0 software interrupt pending
#define CLINT_MTIME      ((volatile uint64_t *)(CLINT_BASE + 0xBFF8)) // Free-running timer

// QEMU virt's mtime frequency (the device tree's timebase-frequency). Unlike cntfrq_el0 on Arm,
// there's no CSR to read it from
#define MTIME_FREQ_HZ    10000000

// mcause values for interrupts (bit 63 set) on RV64
#define MCAUSE_INTERRUPT        (1UL << 63)
#define IRQ_M_SOFT              3  // Machine software interrupt (CLINT MSIP)
#define IRQ_M_EXT               11 // Machine external interrupt (PLIC)

// mstatus/mie bits
#define MSTATUS_MIE             (1UL << 3)
#define MIE_MSIE                (1UL << IRQ_M_SOFT)
#define MIE_MEIE                (1UL << IRQ_M_EXT)

// Shared variable across foreground code and async ISR layers
static volatile uint32_t shared_data = 0;

void uart_puts(const char *s) {
    while (*s) {
        // Wait until UART transmit holding register is empty
        while (!(*UART0_LSR & UART_LSR_THRE));
        *UART0_THR = *s++;
    }
}

void sleep_ms(uint32_t ms) {
    uint64_t ticks_needed = (MTIME_FREQ_HZ / 1000) * (uint64_t)ms;
    uint64_t start = *CLINT_MTIME;
    while ((*CLINT_MTIME - start) < ticks_needed);
}

static inline void enable_interrupts(void)  { asm volatile("csrs mstatus, %0" :: "r"(MSTATUS_MIE) : "memory"); }
static inline void disable_interrupts(void) { asm volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE) : "memory"); }

// Masks interrupts and returns the previous mstatus.MIE, so it can be restored with irq_restore.
// Inside a trap handler interrupts are already masked, and unconditionally re-enabling them would
// allow a nested trap that overwrites mepc/mcause before the handler returns.
static inline uintptr_t irq_save(void) {
    uintptr_t prev;
    asm volatile("csrrc %0, mstatus, %1" : "=r"(prev) : "r"(MSTATUS_MIE) : "memory");
    return prev & MSTATUS_MIE;
}
static inline void irq_restore(uintptr_t prev) { asm volatile("csrs mstatus, %0" :: "r"(prev) : "memory"); }

// "Hardware" interrupt: a real device interrupt through the PLIC. Unlike the GIC, the PLIC has no
// way for software to raise an interrupt, so this enables the UART's "transmit holding register
// empty" interrupt. The THR is empty whenever the UART is idle, so the UART raises it right away
void trigger_hw_irq(void) {
    uintptr_t prev = irq_save();
    *UART0_IER |= UART_IER_ETBEI;
    irq_restore(prev);
}

// "Software" interrupt: writing 1 to this hart's MSIP register raises a machine software interrupt.
// The PLIC only handles device interrupts, so this one comes from the CLINT
void trigger_soft_irq(void) { *CLINT_MSIP = 1; }

// The Unified C-Level Trap Handler, called from trap_entry in startup.S
void handle_trap(uintptr_t mcause) {
    if (!(mcause & MCAUSE_INTERRUPT)) {
        // Synchronous exception (illegal instruction, misaligned/faulting access, ...)
        uart_puts("[TRAP] Unexpected exception, halting\r\n");
        while (1) asm volatile("wfi");
    }

    uintptr_t interrupt_id = mcause & ~MCAUSE_INTERRUPT;

    if (interrupt_id == IRQ_M_EXT) {
        // Read the claim register to get the pending source ID (like reading GICC_IAR)
        uint32_t source = *PLIC_CLAIM0;

        if (source == UART0_IRQ) {
            // Turn the THR empty interrupt back off, otherwise the UART keeps raising it
            *UART0_IER &= ~UART_IER_ETBEI;

            uart_puts("[ISR] 'Hardware' Interrupt tripped! Writing data to shared space...\r\n");
            shared_data = 0xAABBCCDD; // Write to shared variable

            // Instantly trigger our secondary "Software" interrupt. mstatus.MIE is cleared while in
            // the trap handler, so it's taken right after mret returns
            trigger_soft_irq();
        }

        // Signal completion by writing back the claimed ID (like writing GICC_EOIR).
        // Source 0 means there was nothing to claim
        if (source != 0) {
            *PLIC_CLAIM0 = source;
        }
    }
    else if (interrupt_id == IRQ_M_SOFT) {
        *CLINT_MSIP = 0; // Acknowledge: the software interrupt stays pending until MSIP is cleared

        uart_puts("[ISR] 'Software' Interrupt tripped! Processing data inside a Critical Section...\r\n");

        // --- CRITICAL SECTION BEGIN ---
        uintptr_t prev = irq_save(); // Lock out incoming requests safely
        uint32_t local_copy = shared_data;

        // Print proof of successful read
        uart_puts("      Critical Read complete! Value is: 0x");

        // Simple hex print helper routine
        char hex[9];
        for (int i = 7; i >= 0; i--) {
            uint32_t nibble = (local_copy >> (i * 4)) & 0xF;
            hex[7 - i] = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        }
        hex[8] = '\0';
        uart_puts(hex);
        uart_puts("\r\n");

        irq_restore(prev);
        // --- CRITICAL SECTION END ---
    }
}

void plic_init(void) {
    // Give UART0 a non-zero priority (priority 0 means "never interrupt")
    *PLIC_PRIORITY(UART0_IRQ) = 1;

    // Enable UART0 for hart 0's machine-mode context
    *PLIC_ENABLE0 |= (1 << UART0_IRQ);

    // Set the threshold to 0 to let every priority above 0 through (like GICC_PMR = 0xFF)
    *PLIC_THRESHOLD0 = 0;

    *CLINT_MSIP = 0; // No pending software interrupt

    // Enable machine external (PLIC) and machine software (CLINT) interrupts at the hart
    asm volatile("csrs mie, %0" :: "r"(MIE_MEIE | MIE_MSIE));
}

void main_app(void) {
#ifdef NDEBUG
    plic_init();
    enable_interrupts(); // Set mstatus.MIE to take machine-mode interrupts
#endif
    while (1) {
        uart_puts("Hello\r\n");
#ifdef NDEBUG
        sleep_ms(1000);
#endif
        uart_puts("World!\r\n");
#ifdef NDEBUG
        trigger_hw_irq(); // Kick off the hardware -> software cascade
        sleep_ms(1000);
#endif
    }
}
