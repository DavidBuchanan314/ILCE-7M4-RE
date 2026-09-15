FQBN := rp2040:rp2040:waveshare_rp2040_zero
PORT ?= /dev/ttyACM0
SKETCH := .
BUILD_DIR := build
CORE_URL := https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

ARDUINO_CLI  := arduino-cli
ARDUINO_DATA := $(shell $(ARDUINO_CLI) config get directories.data)
PIOASM       := $(shell ls $(ARDUINO_DATA)/packages/rp2040/tools/pqt-pioasm/*/pioasm 2>/dev/null | head -1)

.PHONY: all setup build flash clean

all: build

setup:
	$(ARDUINO_CLI) core update-index --additional-urls $(CORE_URL)
	$(ARDUINO_CLI) core install rp2040:rp2040 --additional-urls $(CORE_URL)

serialboot.pio.h: serialboot.pio
	$(PIOASM) -o c-sdk $< $@

build: serialboot.pio.h
	$(ARDUINO_CLI) compile --fqbn $(FQBN) --build-path $(BUILD_DIR) $(SKETCH)

flash: build
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILD_DIR) $(SKETCH)

clean:
	rm -rf $(BUILD_DIR) serialboot.pio.h
