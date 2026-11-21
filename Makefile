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

CFLAGS  := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -std=gnu11 \
           -Wall -Wextra -ffunction-sections -fdata-sections -MMD -MP
LDFLAGS := -mmcu=$(MCU) -Wl,--gc-sections

# avrdude settings for the Uno bootloader
PORT    ?= COM3
BAUD    ?= 115200

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
	avrdude -p $(MCU) -c arduino -P $(PORT) -b $(BAUD) -U flash:w:$<:i

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
