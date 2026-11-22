# ==============================================================================
# RadixOS - Build System
# Minimalist bare-metal OS for ESP32-S3 (Xtensa LX7)
# ==============================================================================

# Find Espressif Xtensa toolchain if available in standard paths
XTENSA_TOOLCHAIN_DIR ?= $(wildcard /home/john/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin)
ifneq ($(XTENSA_TOOLCHAIN_DIR),)
    CROSS_COMPILE ?= $(firstword $(XTENSA_TOOLCHAIN_DIR))/xtensa-esp32s3-elf-
else
    CROSS_COMPILE ?= xtensa-esp32s3-elf-
endif

CC      := $(CROSS_COMPILE)gcc
AS      := $(CROSS_COMPILE)as
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
SIZE    := $(CROSS_COMPILE)size

HOST_CC ?= gcc
PYTHON  ?= python3
BUILD_DIR := build

# Find RISC-V toolchain for ULP coprocessor (RV32IMC) if available
RISCV_TOOLCHAIN_DIR ?= $(wildcard /home/john/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin)
ifneq ($(RISCV_TOOLCHAIN_DIR),)
    ULP_CC_AUTO := $(firstword $(RISCV_TOOLCHAIN_DIR))/riscv32-esp-elf-gcc
else
    ULP_CC_AUTO := $(shell command -v riscv32-esp-elf-gcc 2>/dev/null || command -v riscv32-unknown-elf-gcc 2>/dev/null || command -v riscv64-unknown-elf-gcc 2>/dev/null)
endif
ULP_CC      ?= $(ULP_CC_AUTO)
ULP_OBJCOPY := $(patsubst %gcc,%objcopy,$(ULP_CC))
ULP_CFLAGS  := -march=rv32imc -mabi=ilp32 -Os -ffreestanding -fno-builtin -nostdlib -nostartfiles -Wall -Wextra -Werror

ifneq ($(strip $(ULP_CC)),)
    ULP_FW_DEP   := $(BUILD_DIR)/ulp_fw_bin.h
    ULP_FW_FLAGS := -I$(BUILD_DIR) -DRADIX_HAVE_ULP_FW_BIN_H
else
    ULP_FW_DEP   :=
    ULP_FW_FLAGS :=
endif

# Optional flashing & emulation tools
ESPTOOL ?= $(shell command -v esptool 2>/dev/null || command -v esptool.py 2>/dev/null || ls /home/john/.espressif/python_env/*/bin/esptool 2>/dev/null | head -n 1)
QEMU_XTENSA ?= $(shell command -v qemu-system-xtensa 2>/dev/null)
ESP32S3_PORT ?= /dev/ttyUSB0
ESP32S3_BAUD ?= 460800
QEMU_MEM_ARGS ?= -nographic -machine esp32s3 -drive file=$(BUILD_DIR)/radixos_flash.bin,if=mtd,format=raw -serial mon:stdio

# Directories
BUILD_DIR := build
INC_DIRS  := include arch/xtensa/include
INCLUDES  := $(addprefix -I, $(INC_DIRS))

# Compiler Flags for Xtensa LX7
CFLAGS := -mlongcalls -mabi=call0 -ffreestanding -fno-builtin -nostdlib \
          -Wall -Wextra -Werror -O2 -g $(INCLUDES)

ASFLAGS := -mlongcalls $(INCLUDES)

LDFLAGS := -T linker/esp32s3_ram.ld -nostdlib -Wl,-Map=$(BUILD_DIR)/radixos.map

# Source files for bare-metal build
SRCS_S := arch/xtensa/boot.S \
          arch/xtensa/boot_core1.S \
          arch/xtensa/vectors.S \
          arch/xtensa/syscall_handler.S \
          arch/xtensa/context_switch.S \
          arch/xtensa/atomic.S

SRCS_C := kernel/main.c \
          kernel/kprintf.c \
          kernel/sched.c \
          kernel/timer.c \
          kernel/sync.c \
          kernel/ipc.c \
          kernel/amp.c \
          kernel/syscall.c \
          kernel/shell.c \
          drivers/system.c \
          drivers/intc.c \
          drivers/ipi.c \
          drivers/world.c \
          drivers/pms.c \
          drivers/ulp.c \
          drivers/uart.c \
          drivers/wdt.c \
          drivers/systimer.c

OBJS := $(addprefix $(BUILD_DIR)/, $(SRCS_S:.S=.o) $(SRCS_C:.c=.o))

# Targets
.PHONY: all clean host-check flash-image qemu flash

all: $(BUILD_DIR)/radixos.bin $(BUILD_DIR)/radixos.elf
	@echo "=== RadixOS Build Complete ==="
	@$(SIZE) $(BUILD_DIR)/radixos.elf

$(BUILD_DIR)/radixos.bin: $(BUILD_DIR)/radixos.elf tools/mk_esp32s3_image.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mk_esp32s3_image.py $< $@

$(BUILD_DIR)/radixos_flash.bin: $(BUILD_DIR)/radixos.elf tools/mk_esp32s3_image.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mk_esp32s3_image.py $< $(BUILD_DIR)/radixos.bin --flash-image $@ --flash-size 4M

flash-image: $(BUILD_DIR)/radixos_flash.bin

qemu: $(BUILD_DIR)/radixos_flash.bin
	@if [ -n "$(QEMU_XTENSA)" ]; then \
		echo "[QEMU] Starting Espressif ESP32-S3 emulation (Ctrl-A X to exit)..."; \
		$(QEMU_XTENSA) $(QEMU_MEM_ARGS); \
	else \
		echo "[QEMU] qemu-system-xtensa not found in PATH."; \
		echo "       Install Espressif QEMU (qemu-system-xtensa) and run:"; \
		echo "       qemu-system-xtensa $(QEMU_MEM_ARGS)"; \
	fi

flash: $(BUILD_DIR)/radixos.bin
	@if [ -n "$(ESPTOOL)" ]; then \
		$(ESPTOOL) --chip esp32s3 --port $(ESP32S3_PORT) --baud $(ESP32S3_BAUD) write-flash 0x0 $<; \
	else \
		echo "[FLASH] esptool not found. Install esptool and run:"; \
		echo "        esptool --chip esp32s3 --port $(ESP32S3_PORT) --baud $(ESP32S3_BAUD) write-flash 0x0 $<"; \
	fi

$(BUILD_DIR)/radixos.elf: $(OBJS) linker/esp32s3_ram.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	@echo "[LD] $@"

$(BUILD_DIR)/ulp_main.elf: ulp/ulp_main.c ulp/ulp.ld
	@mkdir -p $(dir $@)
	$(ULP_CC) $(ULP_CFLAGS) -T ulp/ulp.ld $< -o $@
	@echo "[ULP_CC] $@"

$(BUILD_DIR)/ulp_main.bin: $(BUILD_DIR)/ulp_main.elf
	$(ULP_OBJCOPY) -O binary $< $@
	@echo "[ULP_OBJCOPY] $@"

$(BUILD_DIR)/ulp_fw_bin.h: $(BUILD_DIR)/ulp_main.bin tools/bin2c.py
	$(PYTHON) tools/bin2c.py $< $@ --symbol g_ulp_default_rv32imc_fw --max-size 0x1F00

$(BUILD_DIR)/drivers/ulp.o: drivers/ulp.c $(ULP_FW_DEP)
	@mkdir -p $(dir $@)
	$(if $(strip $(ULP_CC)),,@echo "[ULP] No RISC-V toolchain found; using pre-assembled RV32IMC fallback.")
	$(CC) $(CFLAGS) $(ULP_FW_FLAGS) -c $< -o $@
	@echo "[CC] $<"

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "[AS] $<"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "[CC] $<"

# Host Test Targets (Logic Verification without Xtensa hardware)
HOST_TEST_SCHED_BIN := $(BUILD_DIR)/test_sched_host
HOST_TEST_SCHED_SRCS := tests/test_sched_host.c \
                        kernel/sched.c \
                        kernel/timer.c \
                        kernel/sync.c \
                        kernel/ipc.c \
                        kernel/kprintf.c \
                        drivers/uart.c \
                        drivers/wdt.c \
                        drivers/systimer.c

HOST_TEST_SYNC_IPC_BIN := $(BUILD_DIR)/test_sync_ipc_host
HOST_TEST_SYNC_IPC_SRCS := tests/test_sync_ipc_host.c \
                           kernel/sched.c \
                           kernel/timer.c \
                           kernel/sync.c \
                           kernel/ipc.c \
                           kernel/kprintf.c \
                           drivers/uart.c \
                           drivers/wdt.c \
                           drivers/systimer.c

HOST_TEST_AMP_BIN := $(BUILD_DIR)/test_amp_host
HOST_TEST_AMP_SRCS := tests/test_amp_host.c \
                      kernel/amp.c \
                      kernel/ipc.c \
                      kernel/sync.c \
                      kernel/sched.c \
                      kernel/timer.c \
                      kernel/kprintf.c \
                      drivers/system.c \
                      drivers/ipi.c \
                      drivers/uart.c \
                      drivers/wdt.c \
                      drivers/systimer.c

HOST_TEST_PMS_SYSCALL_BIN := $(BUILD_DIR)/test_pms_syscall_host
HOST_TEST_PMS_SYSCALL_SRCS := tests/test_pms_syscall_host.c \
                              kernel/amp.c \
                              kernel/syscall.c \
                              kernel/sched.c \
                              kernel/timer.c \
                              kernel/ipc.c \
                              kernel/sync.c \
                              kernel/kprintf.c \
                              drivers/world.c \
                              drivers/pms.c \
                              drivers/system.c \
                              drivers/ipi.c \
                              drivers/uart.c \
                              drivers/wdt.c \
                              drivers/systimer.c

HOST_TEST_SHELL_ULP_BIN := $(BUILD_DIR)/test_shell_ulp_host
HOST_TEST_SHELL_ULP_SRCS := tests/test_shell_ulp_host.c \
                            kernel/shell.c \
                            drivers/ulp.c \
                            kernel/amp.c \
                            kernel/syscall.c \
                            kernel/sched.c \
                            kernel/timer.c \
                            kernel/ipc.c \
                            kernel/sync.c \
                            kernel/kprintf.c \
                            drivers/world.c \
                            drivers/pms.c \
                            drivers/system.c \
                            drivers/ipi.c \
                            drivers/uart.c \
                            drivers/wdt.c \
                            drivers/systimer.c

HOST_TEST_INTC_BIN := $(BUILD_DIR)/test_intc_host
HOST_TEST_INTC_SRCS := tests/test_intc_host.c \
                       drivers/intc.c \
                       drivers/system.c \
                       drivers/ipi.c \
                       kernel/amp.c \
                       kernel/sched.c \
                       kernel/timer.c \
                       kernel/ipc.c \
                       kernel/sync.c \
                       kernel/kprintf.c \
                       drivers/uart.c \
                       drivers/wdt.c \
                       drivers/systimer.c

HOST_TEST_TIMERS_UART_BIN := $(BUILD_DIR)/test_timers_uart_host
HOST_TEST_TIMERS_UART_SRCS := tests/test_timers_uart_host.c \
                              kernel/shell.c \
                              drivers/ulp.c \
                              drivers/intc.c \
                              drivers/system.c \
                              drivers/ipi.c \
                              kernel/amp.c \
                              kernel/syscall.c \
                              kernel/sched.c \
                              kernel/timer.c \
                              kernel/ipc.c \
                              kernel/sync.c \
                              kernel/kprintf.c \
                              drivers/world.c \
                              drivers/pms.c \
                              drivers/uart.c \
                              drivers/wdt.c \
                              drivers/systimer.c

host-check: $(ULP_FW_DEP)
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DRADIX_HOST_TEST $(INCLUDES) $(HOST_TEST_SCHED_SRCS) -o $(HOST_TEST_SCHED_BIN)
	@echo "[HOST_CC] Compiled $(HOST_TEST_SCHED_BIN)"
	@echo "Running RadixOS host simulation (Step 1 & Step 2)..."
	@$(HOST_TEST_SCHED_BIN)
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DRADIX_HOST_TEST $(INCLUDES) $(HOST_TEST_SYNC_IPC_SRCS) -o $(HOST_TEST_SYNC_IPC_BIN)
	@echo "[HOST_CC] Compiled $(HOST_TEST_SYNC_IPC_BIN)"
	@echo "Running RadixOS host simulation (Step 3 Atomics, Sync & Zero-Copy IPC)..."
	@$(HOST_TEST_SYNC_IPC_BIN)
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DRADIX_HOST_TEST $(INCLUDES) $(HOST_TEST_AMP_SRCS) -o $(HOST_TEST_AMP_BIN)
	@echo "[HOST_CC] Compiled $(HOST_TEST_AMP_BIN)"
	@echo "Running RadixOS host simulation (Step 4 Asymmetric Multiprocessing & IPIs)..."
	@$(HOST_TEST_AMP_BIN)
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DRADIX_HOST_TEST $(INCLUDES) $(HOST_TEST_PMS_SYSCALL_SRCS) -o $(HOST_TEST_PMS_SYSCALL_BIN)
	@echo "[HOST_CC] Compiled $(HOST_TEST_PMS_SYSCALL_BIN)"
	@echo "Running RadixOS host simulation (Step 5 World Controller, PMS MPU & Syscalls)..."
	@$(HOST_TEST_PMS_SYSCALL_BIN)
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DRADIX_HOST_TEST $(ULP_FW_FLAGS) $(INCLUDES) $(HOST_TEST_SHELL_ULP_SRCS) -o $(HOST_TEST_SHELL_ULP_BIN)
	@echo "[HOST_CC] Compiled $(HOST_TEST_SHELL_ULP_BIN)"
	@echo "Running RadixOS host simulation (Step 6 Shell & RISC-V ULP Coprocessor)..."
	@$(HOST_TEST_SHELL_ULP_BIN)
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DRADIX_HOST_TEST $(INCLUDES) $(HOST_TEST_INTC_SRCS) -o $(HOST_TEST_INTC_BIN)
	@echo "[HOST_CC] Compiled $(HOST_TEST_INTC_BIN)"
	@echo "Running RadixOS host simulation (Interrupt Matrix & Deferred Preemption)..."
	@$(HOST_TEST_INTC_BIN)
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DRADIX_HOST_TEST $(ULP_FW_FLAGS) $(INCLUDES) $(HOST_TEST_TIMERS_UART_SRCS) -o $(HOST_TEST_TIMERS_UART_BIN)
	@echo "[HOST_CC] Compiled $(HOST_TEST_TIMERS_UART_BIN)"
	@echo "Running RadixOS host simulation (Timed Sleep, Software Timers & UART RX IRQ)..."
	@$(HOST_TEST_TIMERS_UART_BIN)

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build artifacts."
