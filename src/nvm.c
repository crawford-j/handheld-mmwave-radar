/*
 * nvm.c - EEPROM read/write through the EEAR / EEDR / EECR registers
 * [M328P §8.4, §8.6.1-8.6.3].
 *
 * Read [§8.6.3, EERE]: wait until no write is in progress (EEPE = 0), put
 * the address in EEAR, set EERE; the byte is in EEDR on the next
 * instruction (the CPU halts 4 cycles).
 *
 * Write [§8.6.3, EEPE procedure]:
 *   1. wait until EEPE = 0 (previous write finished)
 *   2. (SPMEN wait: only needed if the application writes flash; this one
 *      never does, so it is skipped)
 *   3-4. EEAR = address, EEDR = data
 *   5. write EEMPE = 1 (with EEPE = 0)
 *   6. within 4 clock cycles, write EEPE = 1
 * EEPM1:0 stay 00: erase + write in one 3.4 ms operation [Table 8-1].
 * An interrupt between steps 5 and 6 would let EEMPE time out and the write
 * would silently fail, so steps 3-6 run with interrupts disabled. That is
 * only a few cycles; the 3.4 ms wait in step 1 happens with interrupts on,
 * so the tick, buzzer and radar UART keep running.
 *
 * Wear: the EEPROM is rated for 100,000 write/erase cycles per byte [§8.4].
 * nvm_update() reads first and only writes bytes whose value changed.
 */

#include "board.h"
#include <util/atomic.h>
#include "nvm.h"

/*
 * wait_ready - block until any EEPROM write in progress has finished.
 */
static void wait_ready(void)
{
    while (EECR & (1 << EEPE)) {
    }
}

/*
 * nvm_read - read one EEPROM byte.
 */
uint8_t nvm_read(uint16_t addr)
{
    wait_ready();
    EEAR = addr;
    EECR |= (1 << EERE);                /* read strobe */
    return EEDR;
}

/*
 * nvm_update - write one EEPROM byte, only if it differs from what is
 * stored (saves a 3.4 ms write and one of the byte's 100k cycles).
 */
void nvm_update(uint16_t addr, uint8_t value)
{
    if (nvm_read(addr) == value) {
        return;
    }
    wait_ready();
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        EEAR = addr;
        EEDR = value;
        EECR = (1 << EEMPE);            /* step 5: master enable, EEPE = 0, EEPM = 00 */
        EECR |= (1 << EEPE);            /* step 6: start the write (within 4 cycles) */
    }
}
