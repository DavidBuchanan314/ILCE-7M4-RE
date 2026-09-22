# Bare-metal fastboot payload for the ILCE-7M4 AP (CXD90057 "Mira").
# Staged at 0xFE020000 by the bootrom serial-boot protocol; see ../../README.md.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

TARGET := fastboot
BUILD  := build

# -mgeneral-regs-only: FP/SIMD would need CPACR/CPTR set up before first use.
# -mstrict-align: the MMU is off, so every access is Device memory, where
# unaligned accesses fault.
CFLAGS := -march=armv8-a -mgeneral-regs-only -mbranch-protection=none \
          -mstrict-align \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-common \
          -ffunction-sections -fdata-sections \
          -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Iinclude

LDFLAGS := -T $(TARGET).ld -nostdlib -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

# The CP payload is built here and linked in as a blob; cetus.c pushes it.
CETUS_DIR := cetus
CETUS_BIN := $(CETUS_DIR)/build/cetus.bin
CETUS_OBJ := $(BUILD)/cetus_payload.o

SRCS_C := $(wildcard src/*.c)
SRCS_S := $(wildcard src/*.S)
OBJS   := $(patsubst src/%.S,$(BUILD)/%.o,$(SRCS_S)) \
          $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS_C)) \
          $(CETUS_OBJ)

.PHONY: all clean disasm push cetus

all: $(BUILD)/$(TARGET).bin

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "=== $@: $$(stat -c%s $@) bytes ==="

cetus:
	$(MAKE) -C $(CETUS_DIR)

$(CETUS_BIN): cetus

# objcopy names its symbols after the input path, so the blob is copied to a
# fixed name first and converted from inside $(BUILD) to keep them stable.
$(CETUS_OBJ): $(CETUS_BIN) | $(BUILD)
	cp $< $(BUILD)/cetus_payload.bin
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  cetus_payload.bin cetus_payload.o

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
# check is bypassed by never reaching it.
#
# Waits for the payload's own ready line, which arrives over the CP's UART, so
# a failure before the CP is up shows as a missing line and nothing more.
PORT ?= /dev/ttyACM0
READY ?= [mira] fastboot ready
READY_TIMEOUT ?= 5

push: $(BUILD)/$(TARGET).bin
	python3 ../../scripts/push_payload.py -p $(PORT) \
	  --expect '$(READY)' --expect-timeout $(READY_TIMEOUT) \
	  $(BUILD)/$(TARGET).bin

clean:
	rm -rf $(BUILD)
	$(MAKE) -C $(CETUS_DIR) clean
