cmake_minimum_required(VERSION 3.27)

# Cross compiler for bare metal RISC-V (must be set before project()).
# See the README for using Ubuntu's riscv64-linux-gnu-gcc instead
set(CMAKE_C_COMPILER riscv64-unknown-elf-gcc)
set(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(baremetal C ASM)

# RV64IMAC (+ Zicsr for the CSR instructions) with the soft-float lp64 ABI, so no FP registers are
# ever used. medany lets code and data live at 0x80000000 (medlow only reaches +/-2GB around 0)
set(ARCH_FLAGS "-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany")
set(CMAKE_C_FLAGS "${ARCH_FLAGS} -ffreestanding -Wall -Wextra -pedantic")
set(CMAKE_ASM_FLAGS "${ARCH_FLAGS}")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_DEBUG "-Og -ggdb3")
set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG")

# Link without the C library or its startup files, using our own startup code and memory layout
add_executable(baremetal.elf startup.S main.c)
target_link_options(baremetal.elf PRIVATE
    -nostdlib -T ${CMAKE_SOURCE_DIR}/linker.ld)
set_target_properties(baremetal.elf PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/linker.ld)

# -bios none: no OpenSBI firmware, so the program starts in machine mode at 0x80000000.
# The ELF is passed straight to -kernel, so QEMU loads it at its linked address
set(QEMU_COMMAND
    qemu-system-riscv64
    -M virt
    -bios none
    -nographic
    -serial telnet:localhost:5678,server=on,wait=off
    -kernel baremetal.elf)

add_custom_target(qemu
    COMMAND ${QEMU_COMMAND}
    DEPENDS baremetal.elf
    USES_TERMINAL
    VERBATIM
)

add_custom_target(qemu_debug
    COMMAND ${QEMU_COMMAND} -S -s
    DEPENDS baremetal.elf
    USES_TERMINAL
    VERBATIM
)
