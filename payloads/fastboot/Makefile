# Bare-metal fastboot payload for the ILCE-7M4 AP (CXD90057 "Mira").
# Staged at 0xFE020000 by the bootrom serial-boot protocol; see ../../README.md.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

TARGET := fastboot
BUILD  := build

# -mgeneral-regs-only keeps the compiler off FP/SIMD, which would need CPACR/CPTR
# set up before first use. Nothing here needs floating point.
CFLAGS := -march=armv8-a -mgeneral-regs-only -mbranch-protection=none \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-common \
          -ffunction-sections -fdata-sections \
          -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Iinclude

LDFLAGS := -T $(TARGET).ld -nostdlib -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

SRCS_C := $(wildcard src/*.c)
SRCS_S := $(wildcard src/*.S)
OBJS   := $(patsubst src/%.S,$(BUILD)/%.o,$(SRCS_S)) \
          $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS_C))

.PHONY: all clean disasm push

all: $(BUILD)/$(TARGET).bin

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "=== $@: $$(stat -c%s $@) bytes ==="

$(BUILD)/$(TARGET).elf: $(OBJS) $(TARGET).ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

disasm: $(BUILD)/$(TARGET).elf
	$(OBJDUMP) -d $<

# Stage over the RP2040 serial-boot bridge. No unlock word: the ROM's challenge
# check is bypassed by never reaching it (UNLOCK_BYPASS.md).
#
# --no-forward because this payload has no UART transmit side; its only output
# is the LED. Drop the flag once there is something to forward.
PORT ?= /dev/ttyACM0

push: $(BUILD)/$(TARGET).bin
	python3 ../../scripts/push_payload.py -p $(PORT) --no-forward $(BUILD)/$(TARGET).bin

clean:
	rm -rf $(BUILD)
