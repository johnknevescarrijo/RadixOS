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
          kernel/sync.c \
          kernel/ipc.c \
          kernel/amp.c \
          kernel/syscall.c \
          kernel/shell.c \
          drivers/system.c \
          drivers/ipi.c \
          drivers/world.c \
          drivers/pms.c \
          drivers/ulp.c \
          drivers/uart.c \
          drivers/wdt.c \
          drivers/systimer.c

OBJS := $(addprefix $(BUILD_DIR)/, $(SRCS_S:.S=.o) $(SRCS_C:.c=.o))

# Targets
.PHONY: all clean host-check

all: $(BUILD_DIR)/radixos.bin $(BUILD_DIR)/radixos.elf
	@echo "=== RadixOS Build Complete ==="
	@$(SIZE) $(BUILD_DIR)/radixos.elf

$(BUILD_DIR)/radixos.bin: $(BUILD_DIR)/radixos.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) -O binary $< $@
	@echo "[OBJCOPY] $@"

$(BUILD_DIR)/radixos.elf: $(OBJS) linker/esp32s3_ram.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	@echo "[LD] $@"

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
                        kernel/sync.c \
                        kernel/ipc.c \
                        kernel/kprintf.c \
                        drivers/uart.c \
                        drivers/wdt.c \
                        drivers/systimer.c

HOST_TEST_SYNC_IPC_BIN := $(BUILD_DIR)/test_sync_ipc_host
HOST_TEST_SYNC_IPC_SRCS := tests/test_sync_ipc_host.c \
                           kernel/sched.c \
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

host-check:
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

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build artifacts."
