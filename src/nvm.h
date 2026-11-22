/*
 * nvm.h - Register-level EEPROM byte access (ATmega328P, 1 KB EEPROM).
 */

#ifndef NVM_H
#define NVM_H

#include <stdint.h>

uint8_t nvm_read(uint16_t addr);
void    nvm_update(uint16_t addr, uint8_t value);

#endif /* NVM_H */
