/* ADuCM360 Noise/EMF Sensor — v4
 * UART: P0.6=RX, P0.7=TX via DioCfg(0x9000) @ 115200 -> FTDI -> COM5
 * ADC0: AIN0+/AGND- internal 1.2V ref, gain=1, continuous
 * Antenna wire -> AIN0
 */
#include <ADuCM360.h>
#include "UrtLib.h"
#include "DioLib.h"
#include "AdcLib.h"

static void uart_putc(char c)
{
    while (!(UrtLinSta(pADI_UART) & COMLSR_THRE));
    UrtTx(pADI_UART, c);
}
static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
static void uart_puti(int32_t v)
{
    char buf[12]; int i = 0;
    if (v < 0) { uart_putc('-'); v = -v; }
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

void _start(void);
int main(void)
{
    /* Ensure all peripheral clocks enabled (belt-and-braces) */
    pADI_CLKCTL->CLKDIS = 0x0000;

    /* UART on P0.6(RX)/P0.7(TX) — exact register value from every ADI demo */
    DioCfg(pADI_GP0, 0x9000);
    UrtCfg(pADI_UART, 115200, COMLCR_WLS_8BITS, 0);

    uart_puts("$BOOT,ADuCM360\r\n");

    /* ADC0: AIN0 vs AGND, internal 1.2V ref, gain=1, continuous */
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_IDLE);
    AdcRng(pADI_ADC0, ADCCON_ADCREF_INTREF, ADCMDE_PGA_G1, ADCCON_ADCCODE_INT);
    AdcPin(pADI_ADC0, ADCCON_ADCCN_AGND, ADCCON_ADCCP_AIN0);
    AdcFlt(pADI_ADC0, 31, 0, FLT_NORMAL);
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_CONT);

    for (volatile int i = 0; i < 500000; i++);
    uart_puts("$ADC_READY\r\n");

    while (1) {
        if (AdcSta(pADI_ADC0) & ADC0STA_RDY) {
            int32_t raw = (int32_t)AdcRd(pADI_ADC0);
            if (raw & 0x800000) raw |= (int32_t)0xFF000000;
            uart_puts("$ADC,");
            uart_puti(raw);
            uart_puts("\r\n");
        }
    }
    return 0;
}
void _start(void) { main(); }
