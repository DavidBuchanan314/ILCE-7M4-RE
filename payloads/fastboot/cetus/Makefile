# Bare-metal payload for the ILCE-7M4 co-processor (CXD90058 "Cetus").
#
# Not standalone: the parent payload builds this, links it in as a blob and
# pushes it to the CP. It replaces the ROM's SPI monitor with one that speaks
# the same framing plus the flash opcodes.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

TARGET := cetus
BUILD  := build

# Same constraints as the parent: no FP/SIMD, and -mstrict-align because the
# MMU is off.
CFLAGS := -march=armv8-a -mgeneral-regs-only -mbranch-protection=none \
          -mstrict-align \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-common \
          -ffunction-sections -fdata-sections \
          -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Iinclude \
          -MMD -MP

LDFLAGS := -T $(TARGET).ld -nostdlib -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--build-id=none \
           -Wl,--no-warn-rwx-segments

SRCS_C := $(wildcard src/*.c)
SRCS_S := $(wildcard src/*.S)
OBJS   := $(patsubst src/%.S,$(BUILD)/%.o,$(SRCS_S)) \
          $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS_C))

.PHONY: all clean disasm

all: $(BUILD)/$(TARGET).bin

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@sz=$$(stat -c%s $@); pad=$$(( (4 - sz % 4) % 4 )); \
	 if [ $$pad -gt 0 ]; then \
	   dd if=/dev/zero bs=1 count=$$pad >> $@ 2>/dev/null; \
	 fi
	@echo "=== $@: $$(stat -c%s $@) bytes ==="

$(BUILD)/$(TARGET).elf: $(OBJS) $(TARGET).ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

-include $(OBJS:.o=.d)

disasm: $(BUILD)/$(TARGET).elf
	$(OBJDUMP) -d $<

clean:
	rm -rf $(BUILD)
