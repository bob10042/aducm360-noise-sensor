/* UART debug — blinks LED + sends UART on P0.6/P0.7
 * If LED blinks but COM5 silent, the FTDI isn't wired to P0.6/P0.7
 */
#include <ADuCM360.h>
#include "UrtLib.h"
#include "DioLib.h"

static void delay(void) { for (volatile int i = 0; i < 200000; i++); }

void _start(void);
int main(void)
{
    pADI_CLKCTL->CLKDIS = 0x0000;

    /* LED on P0.4 as GPIO output */
    DioCfgPin(pADI_GP0, 4, 0);
    DioOenPin(pADI_GP0, 4, 1);

    /* UART on P0.6/P0.7 — exact ADI demo register value */
    DioCfg(pADI_GP0, 0x9000);
    UrtCfg(pADI_UART, 115200, COMLCR_WLS_8BITS, 0);

    /* Also try brute-force: manually set COMDIV and COMFBR for 115200 @ 16MHz */
    pADI_UART->COMDIV = 4;
    pADI_UART->COMFBR = 0x88AE;

    int count = 0;
    while (1) {
        /* Toggle LED */
        if (count & 1)
            pADI_GP0->GPSET = (1 << 4);
        else
            pADI_GP0->GPCLR = (1 << 4);

        /* Send a byte directly to UART TX register */
        while (!(pADI_UART->COMLSR & 0x20));  /* Wait for THR empty */
        pADI_UART->COMTX = 'U';               /* Send 'U' = 0x55 pattern */

        while (!(pADI_UART->COMLSR & 0x20));
        pADI_UART->COMTX = '\r';

        while (!(pADI_UART->COMLSR & 0x20));
        pADI_UART->COMTX = '\n';

        count++;
        delay();
    }
    return 0;
}
void _start(void) { main(); }
