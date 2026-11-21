# Handheld mmWave radar - ATmega328P @ 16 MHz

MCU     := atmega328p
F_CPU   := 16000000UL
TARGET  := radar

SRC_DIR := src
BUILD   := build
SRCS    := $(wildcard $(SRC_DIR)/*.c)
OBJS    := $(SRCS:$(SRC_DIR)/%.c=$(BUILD)/%.o)

CC      := avr-gcc
OBJCOPY := avr-objcopy
SIZE    := avr-size

# Radar link speed: 250000 (factory radar) or 38400 (after baud switch)
RADAR_BAUD ?= 38400UL
# 1 = build the one-time radar baud switch instead, to RADAR_SETUP_BAUD
RADAR_SETUP ?= 0
RADAR_SETUP_BAUD ?= 38400UL

CFLAGS  := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -DRADAR_BAUD=$(RADAR_BAUD) \
           -DRADAR_SETUP=$(RADAR_SETUP) -DRADAR_SETUP_BAUD=$(RADAR_SETUP_BAUD) \
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

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) --mcu=$(MCU) -C $< 2>/dev/null || $(SIZE) $<

flash: $(BUILD)/$(TARGET).hex
	$(AVRDUDE) -p $(MCU) -c arduino -P $(PORT) -b $(BAUD) -U flash:w:$<:i

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
