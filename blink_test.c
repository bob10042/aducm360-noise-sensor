#include <ADuCM360.h>
#include "DioLib.h"

void _start(void);
int main(void)
{
    pADI_CLKCTL->CLKDIS = 0x0000;
    DioCfgPin(pADI_GP0, 4, 0);
    DioCfgPin(pADI_GP0, 5, 0);
    DioOenPin(pADI_GP0, 4, 1);
    DioOenPin(pADI_GP0, 5, 1);
    while (1) {
        pADI_GP0->GPSET = (1 << 4);
        pADI_GP0->GPCLR = (1 << 5);
        for (volatile int i = 0; i < 300000; i++);
        pADI_GP0->GPCLR = (1 << 4);
        pADI_GP0->GPSET = (1 << 5);
        for (volatile int i = 0; i < 300000; i++);
    }
    return 0;
}
void _start(void) { main(); }
