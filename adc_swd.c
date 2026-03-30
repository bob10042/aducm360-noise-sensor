/* ADC noise sensor — output via SWD only, no UART
 * Read g_adc_val via SWD at address found by: nm adc_swd.elf | grep g_adc
 */
#include <ADuCM360.h>
#include "AdcLib.h"
#include "DioLib.h"

volatile int32_t g_adc_val;
volatile uint32_t g_magic;
volatile uint32_t g_sample_count;

void _start(void);
int main(void)
{
    pADI_CLKCTL->CLKDIS = 0x0000;
    g_adc_val = 0;
    g_magic = 0xCAFE1234;
    g_sample_count = 0;

    /* LED on P0.4 */
    DioCfgPin(pADI_GP0, 4, 0);
    DioOenPin(pADI_GP0, 4, 1);
    pADI_GP0->GPSET = (1 << 4);

    /* ADC0: AIN0 vs AGND, internal 1.2V ref, gain=1, continuous */
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_IDLE);
    AdcRng(pADI_ADC0, ADCCON_ADCREF_INTREF, ADCMDE_PGA_G1, ADCCON_ADCCODE_INT);
    AdcPin(pADI_ADC0, ADCCON_ADCCN_AGND, ADCCON_ADCCP_AIN0);
    AdcFlt(pADI_ADC0, 31, 0, FLT_NORMAL);
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_CONT);

    for (volatile int i = 0; i < 500000; i++);

    while (1) {
        if (AdcSta(pADI_ADC0) & ADC0STA_RDY) {
            int32_t raw = (int32_t)AdcRd(pADI_ADC0);
            if (raw & 0x800000) raw |= (int32_t)0xFF000000;
            g_adc_val = raw;
            g_sample_count++;

            /* Toggle LED every 8 samples */
            if (g_sample_count & 8)
                pADI_GP0->GPSET = (1 << 4);
            else
                pADI_GP0->GPCLR = (1 << 4);
        }
    }
    return 0;
}
void _start(void) { main(); }
