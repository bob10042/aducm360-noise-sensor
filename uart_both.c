/* UART test — sends on BOTH pin pairs, blinks LED
 * P0.6/P0.7 (func1/func2) AND P0.1/P0.2 (func3)
 * Check COM5 and COM6 to see which one gets data
 */
#include <ADuCM360.h>
#include "UrtLib.h"
#include "DioLib.h"

static void delay(void) { for (volatile int i = 0; i < 300000; i++); }

void _start(void);
int main(void)
{
    pADI_CLKCTL->CLKDIS = 0x0000;

    /* LED on P0.4 */
    DioOenPin(pADI_GP0, 4, 1);

    /* Set BOTH UART pin pairs at once:
     * P0.7=func2(TXD), P0.6=func1(RXD), P0.2=func3(TXD), P0.1=func3(RXD)
     * GP0CON = 0x9000 | 0x003C = 0x903C
     * bits[15:14]=10 P0.7 TXD
     * bits[13:12]=01 P0.6 RXD
     * bits[5:4]=11   P0.2 TXD(func3)
     * bits[3:2]=11   P0.1 RXD(func3)
     */
    pADI_GP0->GPCON = 0x903C;

    UrtCfg(pADI_UART, 115200, COMLCR_WLS_8BITS, 0);

    int count = 0;
    while (1) {
        /* Toggle LED */
        if (count & 1)
            pADI_GP0->GPSET = (1 << 4);
        else
            pADI_GP0->GPCLR = (1 << 4);

        /* Send text via UART */
        const char *msg = "$HELLO\r\n";
        while (*msg) {
            while (!(pADI_UART->COMLSR & 0x20));
            pADI_UART->COMTX = *msg++;
        }

        count++;
        delay();
    }
    return 0;
}
void _start(void) { main(); }
