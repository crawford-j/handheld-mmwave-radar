/*
 * spi.c - Hardware SPI master driver (register level).
 *
 * Both devices use SPI mode 0 (CPOL = 0, CPHA = 0), MSB first:
 *   - ILI9341 samples SDA on the rising edge of SCL [ILI9341 §7.1.8].
 *   - XPT2046 latches DIN on the rising edge of DCLK and changes DOUT on the
 *     falling edge [XPT2046 Table 3, Fig. 12].
 *
 * Clock speeds (16 MHz CPU, dividers from [M328P §19.5.1 Table 19-5]):
 *   display: fosc/4  = 4 MHz  (SPI2X=0, SPR1=0, SPR0=0)
 *            ILI9341 write cycle >= 100 ns, i.e. <= 10 MHz [ILI9341 §19.3.4];
 *            4 MHz is the cap set for the TXS0108E level shifter.
 *   touch:   fosc/16 = 1 MHz  (SPI2X=0, SPR1=0, SPR0=1)
 *            XPT2046 tCH, tCL >= 200 ns, i.e. <= 2.5 MHz [XPT2046 Table 9];
 *            the slower clock also lengthens the 3-cycle acquisition time.
 */

#include "board.h"
#include "spi.h"

/*
 * spi_init - configure the SPI pins and enable SPI master at display speed.
 * Also parks both chip selects high (deselected) so neither device sees
 * traffic meant for the other.
 */
void spi_init(void)
{
    /* Deselect both devices before the pins become outputs. */
    LCD_PORT   |= (1 << LCD_CS);              /* display CS idle high */
    TOUCH_PORT |= (1 << TOUCH_CS);            /* touch CS idle high   */
    TOUCH_DDR  |= (1 << TOUCH_CS);            /* touch CS is an output */

    /* MOSI, SCK and SS (= display CS, PB2) as outputs; MISO stays an input.
     * SS must be an output in master mode: if it were an input and went low,
     * the SPI would drop out of master mode [M328P §19.3.2]. */
    SPI_DDR |= (1 << SPI_MOSI) | (1 << SPI_SCK) | (1 << LCD_CS);
    SPI_DDR &= ~(1 << SPI_MISO);

    /* SPE  = 1: enable SPI
     * MSTR = 1: master mode
     * DORD = 0: MSB first; CPOL = 0, CPHA = 0: mode 0
     * SPR1:0 = 00 with SPI2X = 0: fosc/4 [M328P §19.5.1] */
    SPCR = (1 << SPE) | (1 << MSTR);
    SPSR &= ~(1 << SPI2X);
}

/*
 * spi_speed_display - switch SCK to fosc/4 = 4 MHz for the ILI9341.
 */
void spi_speed_display(void)
{
    SPCR &= ~((1 << SPR1) | (1 << SPR0));     /* SPR1:0 = 00 -> fosc/4 */
}

/*
 * spi_speed_touch - switch SCK to fosc/16 = 1 MHz for the XPT2046.
 * Call before asserting touch CS; call spi_speed_display() afterwards.
 */
void spi_speed_touch(void)
{
    SPCR = (SPCR & ~(1 << SPR1)) | (1 << SPR0); /* SPR1:0 = 01 -> fosc/16 */
}

/*
 * spi_xfer - send one byte and return the byte clocked in at the same time.
 * Writing SPDR starts the transfer; SPIF in SPSR sets when it completes
 * [M328P §19.5.2]. Reading SPSR then SPDR clears SPIF.
 */
uint8_t spi_xfer(uint8_t out)
{
    SPDR = out;
    while (!(SPSR & (1 << SPIF))) {
    }
    return SPDR;
}
