# mmWave presence radar - ATmega328P @ 16 MHz

MCU     := atmega328p
F_CPU   := 16000000UL
TARGET  := radar

SRC_DIR := src
BUILD   := build
SRCS    := $(wildcard $(SRC_DIR)/*.c)

# Local standby animation: `python tools/gen_anim.py my.gif --local` writes
# src/local/ (git-ignored, for GIFs that can't go in the public repo). If it
# exists it replaces the built-in src/standby_anim.c.
ANIM_LOCAL := $(wildcard $(SRC_DIR)/local/standby_anim.c)
ifneq ($(ANIM_LOCAL),)
SRCS    := $(filter-out $(SRC_DIR)/standby_anim.c,$(SRCS))
endif
OBJS    := $(SRCS:$(SRC_DIR)/%.c=$(BUILD)/%.o)
ifneq ($(ANIM_LOCAL),)
OBJS    += $(BUILD)/local_standby_anim.o
endif

CC      := avr-gcc
OBJCOPY := avr-objcopy
SIZE    := avr-size

# Radar link speed: 250000 talks to the radar at its factory 256000 (-2.34 %)
RADAR_BAUD ?= 250000UL
# 1 = build the one-time radar baud switch instead, to RADAR_SETUP_BAUD
RADAR_SETUP ?= 0
# 1 = feed synthetic targets instead of the radar (UI testing)
DEMO ?= 0
# 1 = build the touch calibration program instead
TOUCH_CAL ?= 0
RADAR_SETUP_BAUD ?= 38400UL

CFLAGS  := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -DRADAR_BAUD=$(RADAR_BAUD) \
           -DRADAR_SETUP=$(RADAR_SETUP) -DRADAR_SETUP_BAUD=$(RADAR_SETUP_BAUD) \
           -DDEMO=$(DEMO) -DTOUCH_CAL=$(TOUCH_CAL)            -DANIM_LOCAL=$(if $(ANIM_LOCAL),1,0) \
           -Os -std=gnu11 \
           -Wall -Wextra -ffunction-sections -fdata-sections -MMD -MP
LDFLAGS := -mmcu=$(MCU) -Wl,--gc-sections

# avrdude settings for the Uno bootloader
# (defaults to the copy bundled with the Arduino IDE 1.x on Windows)
AVR_TOOLS ?= C:/Program Files (x86)/Arduino/hardware/tools/avr
AVRDUDE   ?= "$(AVR_TOOLS)/bin/avrdude.exe" -C "$(AVR_TOOLS)/etc/avrdude.conf"
PORT      ?= COM3
BAUD      ?= 115200

.PHONY: all clean size flash

all: $(BUILD)/$(TARGET).hex size

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/local_%.o: $(SRC_DIR)/local/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) --mcu=$(MCU) -C $< 2>/dev/null || $(SIZE) $<

# Up to 4 tries: uploads sometimes fail verification, always at byte 47 of a
# 128-byte page. Tests (2026-11-22) showed the bad byte is written, not
# misread (verify-only passes are always clean), and it happens with both
# radar UART wires unplugged, so it isn't the level shifter. Any pass that
# verifies leaves a correct image.
FLASH_CMD = $(AVRDUDE) -p $(MCU) -c arduino -P $(PORT) -b $(BAUD) -U flash:w:$<:i
flash: $(BUILD)/$(TARGET).hex
	$(FLASH_CMD) || $(FLASH_CMD) || $(FLASH_CMD) || $(FLASH_CMD)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
