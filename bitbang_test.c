/* Bit-bang 9600 baud serial on P0.7 (GPIO, no UART peripheral)
 * Also blinks P0.4 LED to confirm running
 * If COM5 shows data at 9600 baud, the switch routing works
 * If silent, the switches aren't routing P0.7 to FTDI
 */
#include <ADuCM360.h>
#include "DioLib.h"

/* ~104us delay for 9600 baud at 16MHz
 * 16MHz = 62.5ns per cycle, 104us / 62.5ns = 1664 cycles
 * Loop overhead ~4 cycles, so count ~400 */
static void bit_delay(void)
{
    for (volatile int i = 0; i < 400; i++);
}

static void bb_putc(char c)
{
    /* Start bit (low) */
    pADI_GP0->GPCLR = (1 << 7);
    bit_delay();

    /* 8 data bits, LSB first */
    for (int i = 0; i < 8; i++) {
        if (c & (1 << i))
            pADI_GP0->GPSET = (1 << 7);
        else
            pADI_GP0->GPCLR = (1 << 7);
        bit_delay();
    }

    /* Stop bit (high) */
    pADI_GP0->GPSET = (1 << 7);
    bit_delay();
    bit_delay();  /* extra stop time */
}

static void bb_puts(const char *s)
{
    while (*s) bb_putc(*s++);
}

void _start(void);
int main(void)
{
    pADI_CLKCTL->CLKDIS = 0x0000;

    /* P0.4 = LED output, P0.7 = bit-bang TX output */
    /* Keep all pins as GPIO (function 0) */
    pADI_GP0->GPCON = 0x0000;
    pADI_GP0->GPOEN = (1 << 4) | (1 << 7);  /* P0.4 and P0.7 as outputs */
    pADI_GP0->GPSET = (1 << 7);  /* TX idle high */

    int count = 0;
    while (1) {
        /* Toggle LED */
        if (count & 1)
            pADI_GP0->GPSET = (1 << 4);
        else
            pADI_GP0->GPCLR = (1 << 4);

        bb_puts("$BB,OK\r\n");

        for (volatile int i = 0; i < 100000; i++);
        count++;
    }
    return 0;
}
void _start(void) { main(); }
