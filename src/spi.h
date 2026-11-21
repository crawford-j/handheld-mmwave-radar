/*
 * spi.h - Hardware SPI master shared by the ILI9341 display and XPT2046 touch.
 */

#ifndef SPI_H
#define SPI_H

#include <stdint.h>
#include <avr/io.h>

void    spi_init(void);
void    spi_speed_display(void);
void    spi_speed_touch(void);
uint8_t spi_xfer(uint8_t out);

/*
 * spi_write - send one byte, ignoring what comes back. Inline so the display
 * driver's inner loops avoid a function call per byte: at 4 MHz a byte
 * takes 2 us (32 cycles) on the wire, and a call/return plus the extra
 * SPDR read cost ~10-15 more cycles each time.
 * Writing SPDR starts the transfer; SPIF sets when it is done [M328P §19.5.2].
 * SPDR is never read here: SPIF is cleared by reading SPSR with SPIF set
 * (the wait loop) and then accessing SPDR, which the next write does.
 */
static inline void spi_write(uint8_t out)
{
    SPDR = out;
    while (!(SPSR & (1 << SPIF))) {
    }
}

#endif /* SPI_H */
