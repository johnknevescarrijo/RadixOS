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

# Source files for bare-metal build (Step 1)
SRCS_S := arch/xtensa/boot.S \
          arch/xtensa/vectors.S

SRCS_C := kernel/main.c \
          kernel/kprintf.c \
          drivers/uart.c \
          drivers/wdt.c

OBJS := $(addprefix $(BUILD_DIR)/, $(SRCS_S:.S=.o) $(SRCS_C:.c=.o))

# Targets
.PHONY: all clean

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

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build artifacts."
