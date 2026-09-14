FQBN := rp2040:rp2040:waveshare_rp2040_zero
PORT ?= /dev/ttyACM0
SKETCH := .
BUILD_DIR := build
CORE_URL := https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

.PHONY: all setup build flash clean

all: build

setup:
	arduino-cli core update-index --additional-urls $(CORE_URL)
	arduino-cli core install rp2040:rp2040 --additional-urls $(CORE_URL)

build:
	arduino-cli compile --fqbn $(FQBN) --build-path $(BUILD_DIR) $(SKETCH)

flash: build
	arduino-cli upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILD_DIR) $(SKETCH)

clean:
	rm -rf $(BUILD_DIR)
