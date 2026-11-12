# Bare metal C example for QEMU RISC-V "virt" machine (RV64IMAC)

It prints over the UART, and in release builds a device interrupt through the PLIC ("hardware") triggers a software interrupt, which reads shared data inside a critical section.

## Prerequisites

```
sudo apt install gcc-riscv64-unknown-elf qemu-system-riscv cmake gdb-multiarch
```

## Build and run

```
cmake -S . -B build
cmake --build build --target qemu       # connect with `telnet localhost 5678`
cmake --build build --target qemu_debug # debug
```

Add `-DCMAKE_BUILD_TYPE=Debug` for a `-Og -ggdb3` build, which, like the Arm version, skips the interrupts and sleeps so every line can be stepped through. `qemu_debug` starts QEMU paused with a GDB server on `localhost:1234`; attach with the VS Code "C: Remote Attach" configuration, or `gdb-multiarch build/baremetal.elf -ex "target remote :1234"`.

## How it maps to the Arm version

| Arm (Cortex-A53, EL1) | RISC-V (RV64IMAC, machine mode) |
|---|---|
| PL011 UART at 0x09000000 | NS16550A UART at 0x10000000 |
| `cntfrq_el0` / `cntpct_el0` | CLINT `mtime` at 0x0200BFF8, fixed 10 MHz on QEMU virt (there's no CSR holding the frequency) |
| GICv2 distributor + CPU interface | PLIC at 0x0C000000 (context 0 = hart 0 in machine mode) |
| SGI 1 ("hardware" interrupt) | UART0 (PLIC source 10) "transmit holding register empty" interrupt |
| SGI 2 ("software" interrupt) | CLINT machine software interrupt, raised by writing 1 to `msip` |
| Read `GICC_IAR` / write `GICC_EOIR` | Read / write the PLIC claim/complete register; clear `msip` for the software interrupt |
| `GICC_PMR = 0xFF` | PLIC threshold 0 (and a non-zero priority for UART0) |
| `vbar_el1` + 16-entry vector table | `mtvec` in direct mode: one `trap_entry` for every trap, which checks `mcause` |
| `DAIF.I` (`msr daifset/daifclr`) | `mstatus.MIE` (`csrc`/`csrs`), plus per-source enables in `mie` |
| `eret` | `mret` |
| `mpidr_el1` core check | `mhartid` hart check |

Differences worth knowing:

- **The PLIC can't raise interrupts from software.** The GIC has SGIs for that, but the PLIC only forwards interrupts from devices. So the "hardware" interrupt comes from a real device: `trigger_hw_irq` enables the UART's "transmit holding register empty" interrupt, which the idle UART raises right away. The handler claims it from the PLIC, turns that UART interrupt back off, and completes it. The "software" interrupt has to come from the CLINT instead, since the PLIC doesn't handle those.
- **No firmware.** `-bios none` skips OpenSBI, so the program runs in machine mode starting at the RAM base 0x80000000, and the ELF is passed straight to `-kernel`. (With the default OpenSBI, you'd run in supervisor mode at 0x80200000, use PLIC context 1, and use SBI calls for software interrupts.)
- **The critical section saves and restores `mstatus.MIE`** instead of unconditionally re-enabling interrupts. It runs inside the trap handler, where interrupts are already masked, and turning them back on there could allow a nested trap that overwrites `mepc`/`mcause` before `trap_entry` returns.
- **`gp` (global pointer)** is set in `startup.S` from `__global_pointer$` in `linker.ld`. The linker uses it to turn accesses to small globals into single instructions.
- **No FP registers.** RV64IMAC has no F/D extensions and the ABI is `lp64` (soft-float), so `trap_entry` only saves integer registers. This is the RISC-V equivalent of `-mgeneral-regs-only`.

## Compiler flags and the Ubuntu Linux cross compiler

The build uses Ubuntu's bare-metal `riscv64-unknown-elf-gcc` (`gcc-riscv64-unknown-elf` package). Despite the `riscv64` name, it includes 32-bit and 64-bit libgcc builds for many `-march`/`-mabi` combinations, including `rv64imac/lp64`. It doesn't include a C library, which doesn't matter here since the build uses `-nostdlib`.

These flags are required:

- `-march=rv64imac_zicsr -mabi=lp64`: RV64IMAC with the soft-float ABI. `zicsr` is needed for the `csr*` instructions.
- `-mcmodel=medany`: the default `medlow` code model can only address within 2 GB of address 0, which doesn't reach 0x80000000.
- `-ffreestanding`: there's no hosted C library or normal `main` startup.
- `-nostdlib`: don't link a C library or startup files (`crt0.o`).
- `-T linker.ld`: sets the memory layout and the 0x80000000 load address.

You could also build with Ubuntu's Linux cross compiler, `riscv64-linux-gnu-gcc` (`gcc-riscv64-linux-gnu` package). Set `CMAKE_C_COMPILER`/`CMAKE_ASM_COMPILER` to `riscv64-linux-gnu-gcc`, delete `build/` and reconfigure, and add flags that undo its Linux defaults:

- `-fno-pie` (compile) and `-static -no-pie` (link): it builds position-independent executables by default.
- `-Wl,--build-id=none`: it tells the linker to add a `.note.gnu.build-id` section.

With `riscv64-unknown-elf-gcc` those flags are redundant: the output binary is byte-for-byte identical with or without them.

The catch with `riscv64-linux-gnu-gcc` is that its libgcc is built only for its default target, a much larger RVA23-style `-march` with the `lp64d` hard-float ABI, so it can't be linked into an `rv64imac/lp64` program. This example still works because RV64IMAC has hardware multiply/divide and the code uses no floating point, so nothing calls into libgcc. Code that needs libgcc helpers (e.g. floating point on RV64IMAC like `__adddf3`, or `__clzdi2`) fails to link with `riscv64-linux-gnu-gcc` but works with `riscv64-unknown-elf-gcc` (add `-lgcc` after `-nostdlib`).

## Notes

- QEMU's default `rv64` CPU implements many more extensions than RV64IMAC. That doesn't matter, since the compiler only emits RV64IMAC instructions (check with `riscv64-unknown-elf-objdump -d build/baremetal.elf`)
- If a watchdog timer is desired, the Intel 6300ESB `-device i6300esb` PCIe device is available to add to the `qemu-system-riscv64` command. With `-bios none` nothing sets up PCI, so the program has to find it through the PCIe ECAM at 0x30000000 and place its BAR in the PCI memory window at 0x40000000. QEMU doesn't implement its stage 1 interrupt, so it can only reset the machine
- If you want to read UART messages from a /dev file, add `-serial pty` to the `qemu-system-riscv64` command, then run `screen /dev/pts/?` where ? is the value given by QEMU
