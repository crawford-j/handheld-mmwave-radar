/*
 * spi.h - Hardware SPI master shared by the ILI9341 display and XPT2046 touch.
 */

#ifndef SPI_H
#define SPI_H

#include <stdint.h>

void    spi_init(void);
void    spi_speed_display(void);
void    spi_speed_touch(void);
uint8_t spi_xfer(uint8_t out);

#endif /* SPI_H */
