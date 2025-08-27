# SPDX-License-Identifier: Apache-2.0

set(SUPPORTED_EMU_PLATFORMS qemu)

set(QEMU_binary_suffix riscv64)
set(QEMU_CPU_TYPE_${ARCH} riscv64)

set(QEMU_FLAGS_${ARCH}
  -nographic
  -machine virt,aia=aplic-imsic
  -bios none
  -m 256
  -smp 4
  # Enhanced debug options for AIA/IMSIC debugging
  -d cpu_reset,guest_errors,unimp,int
  -D /tmp/qemu_debug.log
  # Enable AIA trace events
  -trace enable=riscv_aplic_*
  -trace enable=riscv_imsic_*
  -trace enable=riscv_trap
  # Increase verbosity for debugging
  -global riscv_aplic.trace_events=1
  -global riscv_imsic.trace_events=1
  )
board_set_debugger_ifnset(qemu)
