/* ADuCM360 Full Firmware Stack v2.0.0
 * EVAL-ADICUP360 Rev 1.1
 *
 * Peripherals active:
 *   ADC0  — AIN0..AIN11 round-robin scan, single-shot per channel, IRQ
 *   ADC1  — Internal temp sensor, continuous, IRQ
 *   DAC   — 12-bit sawtooth waveform
 *   Timer0 — 1ms tick, heartbeat, GPIO snapshot
 *   WDT   — ~10s watchdog, reset mode
 *   UART  — 115200 on P0.6/P0.7
 *   SPI1  — Master, 1MHz, mode 0, on P0.0-P0.3
 *   GPIO  — P0.4 heartbeat LED, P0.5 ADC LED, P1.0-P1.7 inputs, P2.0-P2.3 inputs
 *   Flash — Calibration storage at page 0x1F800
 *
 * SWD: g_dbg @ auto-detected, g_ain[12] immediately after
 * Build: bash build.sh firmware_full
 * Flash: python flash.py firmware_full.bin
 * View:  python viewer_swd_full.py
 */
#include <ADuCM360.h>
#include "AdcLib.h"
#include "DioLib.h"
#include "DmaLib.h"
#include "DacLib.h"
#include "GptLib.h"
#include "WdtLib.h"
#include "UrtLib.h"
#include "ClkLib.h"
#include "IntLib.h"
#include "FeeLib.h"
#include "SpiLib.h"

/* ------------------------------------------------------------------ */
/*  SWD Debug Struct — 128 bytes (32 words), read by viewer           */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed, aligned(4))) {
    /* Header (16 bytes) */
    uint32_t magic;              /* 0x00: 0xDEAD360F */
    uint32_t fw_version;         /* 0x04: 0x00020000 = v2.0.0 */
    uint32_t uptime_sec;         /* 0x08: seconds since boot */
    uint32_t status_flags;       /* 0x0C: bitfield */

    /* ADC0 block — current channel (24 bytes) */
    int32_t  adc0_raw;           /* 0x10: latest reading (active channel) */
    int32_t  adc0_min;           /* 0x14: minimum across all channels */
    int32_t  adc0_max;           /* 0x18: maximum across all channels */
    uint32_t adc0_count;         /* 0x1C: total samples (all channels) */
    uint32_t adc0_rate;          /* 0x20: samples/sec (all channels) */
    uint32_t adc0_errors;        /* 0x24: ADC0 error count */

    /* ADC1 block — temperature (24 bytes) */
    int32_t  adc1_raw;           /* 0x28: latest temp reading */
    int32_t  adc1_min;           /* 0x2C */
    int32_t  adc1_max;           /* 0x30 */
    uint32_t adc1_count;         /* 0x34 */
    uint32_t adc1_rate;          /* 0x38 */
    uint32_t adc1_errors;        /* 0x3C */

    /* DAC block (8 bytes) */
    uint32_t dac_value;          /* 0x40: 12-bit DAC output */
    uint32_t dac_step;           /* 0x44: sawtooth counter */

    /* System block (24 bytes) */
    uint32_t tick_ms;            /* 0x48 */
    uint32_t timer_overflows;    /* 0x4C */
    uint32_t wdt_pets;           /* 0x50 */
    uint32_t gpio_p0_out;        /* 0x54: P0 output register */
    uint32_t loop_count;         /* 0x58 */
    uint32_t uart_tx_count;      /* 0x5C */

    /* Flash calibration (8 bytes) */
    uint32_t cal_valid;          /* 0x60: 0xCA1B0000 if valid */
    int32_t  cal_adc0_offset;    /* 0x64 */

    /* Error / command (8 bytes) */
    uint32_t error_total;        /* 0x68 */
    uint32_t swd_command;        /* 0x6C: 0x01=save cal, 0x02=reset minmax,
                                            0x03=reset DAC, 0xFF=reset */

    /* Multi-channel info (16 bytes) */
    uint32_t adc0_chan;          /* 0x70: currently sampling AIN channel (0-11) */
    uint32_t adc0_scans;         /* 0x74: complete 12-channel scan cycles */
    uint32_t gpio_p1_in;         /* 0x78: P1.0-P1.7 input state */
    uint32_t gpio_p2_in;         /* 0x7C: P2.0-P2.3 input state */
} DbgStruct;

/* Status flag bits */
#define STS_ADC0_RUNNING   (1U << 0)
#define STS_ADC1_RUNNING   (1U << 1)
#define STS_DAC_RUNNING    (1U << 2)
#define STS_UART_OK        (1U << 3)
#define STS_WDT_ACTIVE     (1U << 4)
#define STS_TIMER_RUNNING  (1U << 5)
#define STS_CAL_VALID      (1U << 6)
#define STS_FLASH_BUSY     (1U << 7)
#define STS_BOOT_COMPLETE  (1U << 8)
#define STS_SPI1_READY     (1U << 9)
#define STS_GPIO_READY     (1U << 10)

/* ------------------------------------------------------------------ */
/*  Globals                                                           */
/* ------------------------------------------------------------------ */
volatile DbgStruct g_dbg;

/* Per-channel AIN readings — 12 channels × 4 bytes = 48 bytes
 * Placed immediately after g_dbg in BSS by linker.
 * Viewer reads at g_dbg_addr + 128. */
volatile int32_t g_ain[12];

/* UART RX loopback counter */
volatile uint32_t g_uart_rx_count  = 0;
volatile uint32_t g_uart_rx_errors = 0;

/* ISR-shared temporaries */
static volatile uint32_t s_adc0_count_prev;
static volatile uint32_t s_adc1_count_prev;
static volatile uint8_t  s_ain_chan;        /* current AIN channel 0-11 */

/* AIN positive mux values — ADCCON_ADCCP_AINn = (n << 5) */
static const uint16_t s_ain_mux[12] = {
    (0  << 5), (1  << 5), (2  << 5), (3  << 5),
    (4  << 5), (5  << 5), (6  << 5), (7  << 5),
    (8  << 5), (9  << 5), (10 << 5), (11 << 5)
};

/* Flash calibration */
#define CAL_FLASH_ADDR  0x0001F800
#define CAL_MAGIC       0xCA1B0000

/* ------------------------------------------------------------------ */
/*  UART helpers                                                       */
/* ------------------------------------------------------------------ */
static void uart_putc(char c)
{
    volatile int t = 5000;
    while (!(UrtLinSta(pADI_UART) & COMLSR_THRE) && --t > 0);
    if (t > 0) { UrtTx(pADI_UART, c); g_dbg.uart_tx_count++; }
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
static void uart_putu(uint32_t v)
{
    char buf[12]; int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}
static void uart_puthex(uint32_t v)
{
    const char h[] = "0123456789ABCDEF";
    uart_putc('0'); uart_putc('x');
    for (int i = 28; i >= 0; i -= 4) uart_putc(h[(v >> i) & 0xF]);
}

/* ------------------------------------------------------------------ */
/*  Flash calibration                                                 */
/* ------------------------------------------------------------------ */
static void cal_read(void)
{
    uint32_t *p = (uint32_t *)CAL_FLASH_ADDR;
    if (p[0] == CAL_MAGIC) {
        g_dbg.cal_valid      = CAL_MAGIC;
        g_dbg.cal_adc0_offset = (int32_t)p[1];
        g_dbg.status_flags   |= STS_CAL_VALID;
        uart_puts("$CAL,LOADED,offset="); uart_puti(g_dbg.cal_adc0_offset);
        uart_puts("\r\n");
    } else {
        g_dbg.cal_valid = 0;
        uart_puts("$CAL,NONE\r\n");
    }
}
static void cal_write(void)
{
    g_dbg.status_flags |= STS_FLASH_BUSY;
    uart_puts("$CAL,SAVING...\r\n");
    FeeWrEn(1);
    FeePErs(CAL_FLASH_ADDR);
    while (FeeSta() & 1);
    uint32_t *dst = (uint32_t *)CAL_FLASH_ADDR;
    dst[0] = CAL_MAGIC; while (FeeSta() & 1);
    dst[1] = (uint32_t)g_dbg.cal_adc0_offset; while (FeeSta() & 1);
    FeeWrEn(0);
    g_dbg.status_flags &= ~STS_FLASH_BUSY;
    if (*(volatile uint32_t *)CAL_FLASH_ADDR == CAL_MAGIC) {
        g_dbg.cal_valid = CAL_MAGIC;
        g_dbg.status_flags |= STS_CAL_VALID;
        uart_puts("$CAL,SAVED,OK\r\n");
    } else {
        uart_puts("$CAL,SAVED,FAIL\r\n");
        g_dbg.error_total++;
    }
}

/* ------------------------------------------------------------------ */
/*  Peripheral init                                                   */
/* ------------------------------------------------------------------ */
static void init_clocks(void)
{
    pADI_CLKCTL->CLKDIS = 0x0000;  /* all peripheral clocks on */
    uart_puts("$INIT,CLOCKS,ALL_ENABLED\r\n");
}

static void init_gpio(void)
{
    /* P0 GPCON = 0x9055
     * P0.0=MISO1(01) P0.1=SCLK1(01) P0.2=MOSI1(01) P0.3=CS1_N(01)
     * P0.4=GPIO(00)  P0.5=GPIO(00)  P0.6=SIN/RX(01) P0.7=SOUT/TX(10)
     * = 0b10_01_00_00_01_01_01_01 = 0x9055 */
    DioCfg(pADI_GP0, 0x9055);

    /* P0.4 = LED heartbeat output */
    DioOenPin(pADI_GP0, 4, 1);
    pADI_GP0->GPSET = (1 << 4);

    /* P0.5 = LED ADC activity output */
    DioOenPin(pADI_GP0, 5, 1);
    pADI_GP0->GPCLR = (1 << 5);

    /* P1.0-P1.7 = GPIO inputs (all func0, output disabled) */
    DioCfg(pADI_GP1, 0x0000);
    /* No DioOen = all inputs by default */

    /* P2.0-P2.3 = GPIO inputs */
    DioCfg(pADI_GP2, 0x0000);

    g_dbg.status_flags |= STS_GPIO_READY;
    uart_puts("$INIT,GPIO,P0.4=LED_HB,P0.5=LED_ADC,P1=INPUTS,P2=INPUTS\r\n");
}

static void init_uart(void)
{
    /* P0.6/P0.7 already configured in init_gpio GPCON = 0x9055 */
    UrtCfg(pADI_UART, B115200, COMLCR_WLS_8BITS, 0);
    g_dbg.status_flags |= STS_UART_OK;
    uart_puts("\r\n========================================\r\n");
    uart_puts("  ADuCM360 Full Firmware v2.0.0\r\n");
    uart_puts("  EVAL-ADICUP360 Rev1.1\r\n");
    uart_puts("  12xAIN + SPI1 + Full GPIO\r\n");
    uart_puts("========================================\r\n");
    uart_puts("$BOOT,ADuCM360,FW_FULL,v2.0.0\r\n");
    uart_puts("$INIT,UART,115200,8N1,P0.6_RX,P0.7_TX\r\n");
}

static void init_spi1(void)
{
    /* P0.0-P0.3 already set to SPI1 func1 in init_gpio GPCON = 0x9055
     * SpiBaud: clkdiv=9 → ~1.6MHz at 16MHz UCLK
     * Mode 0: CPOL=LOW, CPHA=sample on leading edge */
    SpiBaud(pADI_SPI1, 9, SPIDIV_BCRST_DIS);
    SpiCfg(pADI_SPI1, SPICON_MOD_TX1RX1, SPICON_MASEN_EN,
           SPICON_CON_EN | SPICON_SOEN_EN | SPICON_ZEN_EN |
           SPICON_TIM_TXWR | SPICON_ENABLE_EN);
    g_dbg.status_flags |= STS_SPI1_READY;
    uart_puts("$INIT,SPI1,MASTER,1.6MHz,MODE0,P0.0=MISO,P0.1=SCLK,P0.2=MOSI,P0.3=CS\r\n");
}

static void init_adc0(void)
{
    /* ADC0: round-robin AIN0-AIN11, single-shot per channel, 1.2V ref, G1 */
    s_ain_chan = 0;
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_IDLE);
    AdcRng(pADI_ADC0, ADCCON_ADCREF_INTREF, ADCMDE_PGA_G1, ADCCON_ADCCODE_INT);
    AdcPin(pADI_ADC0, ADCCON_ADCCN_AGND, s_ain_mux[0]);
    AdcFlt(pADI_ADC0, 31, 0, FLT_NORMAL);
    AdcMski(pADI_ADC0, ADCMSKI_RDY, 1);
    NVIC_EnableIRQ(ADC0_IRQn);
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_SINGLE);  /* kick off first conversion */

    g_dbg.adc0_min = (int32_t)0x0FFFFFFF;
    g_dbg.adc0_max = (int32_t)0xF0000000;
    g_dbg.status_flags |= STS_ADC0_RUNNING;
    uart_puts("$INIT,ADC0,AIN0-AIN11_SCAN,1.2Vref,G1,SF31,SINGLE_IRQ\r\n");
}

static void init_adc1(void)
{
    AdcGo(pADI_ADC1, ADCMDE_ADCMD_IDLE);
    AdcRng(pADI_ADC1, ADCCON_ADCREF_INTREF, ADCMDE_PGA_G1, ADCCON_ADCCODE_INT);
    AdcPin(pADI_ADC1, ADCCON_ADCCN_AGND, ADCCON_ADCCP_TEMP);
    AdcFlt(pADI_ADC1, 127, 0, FLT_NORMAL);
    AdcMski(pADI_ADC1, ADCMSKI_RDY, 1);
    NVIC_EnableIRQ(ADC1_IRQn);
    AdcGo(pADI_ADC1, ADCMDE_ADCMD_CONT);

    g_dbg.adc1_min = (int32_t)0x0FFFFFFF;
    g_dbg.adc1_max = (int32_t)0xF0000000;
    g_dbg.status_flags |= STS_ADC1_RUNNING;
    uart_puts("$INIT,ADC1,TEMP_SENSOR,1.2Vref,G1,SF127,CONT,IRQ\r\n");
}

static void init_dac(void)
{
    DacCfg(DACCON_CLR_Off, DACCON_RNG_IntVref, DACCON_CLK_HCLK, DACCON_MDE_12bit);
    DacWr(0, 0);
    g_dbg.dac_value = 0;
    g_dbg.dac_step  = 0;
    g_dbg.status_flags |= STS_DAC_RUNNING;
    uart_puts("$INIT,DAC,12bit,IntVref\r\n");
}

static void init_timer0(void)
{
    GptLd(pADI_TM0, 62);
    GptCfg(pADI_TM0, TCON_CLK_UCLK, TCON_PRE_DIV256,
           TCON_MOD_PERIODIC | TCON_UP_DIS | TCON_RLD_EN | TCON_ENABLE_EN);
    NVIC_EnableIRQ(TIMER0_IRQn);
    g_dbg.status_flags |= STS_TIMER_RUNNING;
    uart_puts("$INIT,TIMER0,1ms,UCLK/256,IRQ\r\n");
}

static void init_wdt(void)
{
    WdtCfg(T3CON_PRE_DIV16, T3CON_IRQ_DIS, T3CON_PD_DIS);
    WdtLd(20480);
    WdtGo(T3CON_ENABLE_EN);
    g_dbg.status_flags |= STS_WDT_ACTIVE;
    uart_puts("$INIT,WDT,10s,RESET_MODE\r\n");
}

/* ------------------------------------------------------------------ */
/*  ISRs                                                              */
/* ------------------------------------------------------------------ */

/* ADC0 ISR — fires after each single-shot, then chains to next channel */
void ADC0_Int_Handler(void)
{
    int32_t raw = (int32_t)AdcRd(pADI_ADC0);
    /* AdcRd returns 28-bit signed in bits 27:0, sign in bits 31:28 — no
     * further sign extension needed */

    /* Apply calibration offset (only on AIN0, the noise/antenna channel) */
    if (s_ain_chan == 0 && g_dbg.cal_valid == CAL_MAGIC)
        raw -= g_dbg.cal_adc0_offset;

    /* Store per-channel result */
    g_ain[s_ain_chan] = raw;

    /* Update global tracking (cross-channel min/max, count) */
    g_dbg.adc0_raw  = raw;
    g_dbg.adc0_chan = s_ain_chan;
    g_dbg.adc0_count++;

    if (raw < g_dbg.adc0_min) g_dbg.adc0_min = raw;
    if (raw > g_dbg.adc0_max) g_dbg.adc0_max = raw;

    /* ADC activity LED every 16 total samples */
    if (g_dbg.adc0_count & 16) pADI_GP0->GPSET = (1 << 5);
    else                        pADI_GP0->GPCLR = (1 << 5);

    /* Advance to next channel */
    s_ain_chan++;
    if (s_ain_chan >= 12) {
        s_ain_chan = 0;
        g_dbg.adc0_scans++;
    }

    /* Reconfigure mux and trigger next single conversion */
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_IDLE);
    AdcPin(pADI_ADC0, ADCCON_ADCCN_AGND, (int)s_ain_mux[s_ain_chan]);
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_SINGLE);
}

/* ADC1 ISR — temperature */
void ADC1_Int_Handler(void)
{
    int32_t raw = (int32_t)AdcRd(pADI_ADC1);
    g_dbg.adc1_raw = raw;
    g_dbg.adc1_count++;
    if (raw < g_dbg.adc1_min) g_dbg.adc1_min = raw;
    if (raw > g_dbg.adc1_max) g_dbg.adc1_max = raw;
}

/* Timer0 ISR — 1ms tick */
void GP_Tmr0_Int_Handler(void)
{
    GptClrInt(pADI_TM0, TSTA_TMOUT);
    g_dbg.tick_ms++;
    g_dbg.timer_overflows++;

    if ((g_dbg.tick_ms % 1000) == 0) {
        g_dbg.uptime_sec++;

        g_dbg.adc0_rate = g_dbg.adc0_count - s_adc0_count_prev;
        s_adc0_count_prev = g_dbg.adc0_count;

        g_dbg.adc1_rate = g_dbg.adc1_count - s_adc1_count_prev;
        s_adc1_count_prev = g_dbg.adc1_count;

        /* Heartbeat LED */
        if (g_dbg.uptime_sec & 1) pADI_GP0->GPSET = (1 << 4);
        else                       pADI_GP0->GPCLR = (1 << 4);

        /* Snapshot GPIO */
        g_dbg.gpio_p0_out = pADI_GP0->GPOUT;
        g_dbg.gpio_p1_in  = pADI_GP1->GPIN;
        g_dbg.gpio_p2_in  = pADI_GP2->GPIN;
    }
}

/* ------------------------------------------------------------------ */
/*  SWD command processor                                             */
/* ------------------------------------------------------------------ */
static void process_swd_commands(void)
{
    uint32_t cmd = g_dbg.swd_command;
    if (!cmd) return;
    g_dbg.swd_command = 0;

    switch (cmd) {
    case 0x01:
        uart_puts("$CMD,SAVE_CAL\r\n");
        g_dbg.cal_adc0_offset = g_ain[0];  /* zero-cal on AIN0 */
        cal_write();
        break;
    case 0x02:
        uart_puts("$CMD,RESET_MINMAX\r\n");
        g_dbg.adc0_min = (int32_t)0x0FFFFFFF;
        g_dbg.adc0_max = (int32_t)0xF0000000;
        g_dbg.adc1_min = (int32_t)0x0FFFFFFF;
        g_dbg.adc1_max = (int32_t)0xF0000000;
        break;
    case 0x03:
        uart_puts("$CMD,RESET_DAC\r\n");
        g_dbg.dac_value = 0; g_dbg.dac_step = 0;
        DacWr(0, 0);
        break;
    case 0xFF:
        uart_puts("$CMD,RESET!\r\n");
        NVIC_SystemReset();
        break;
    default:
        uart_puts("$CMD,UNK,"); uart_puthex(cmd); uart_puts("\r\n");
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  UART telemetry                                                    */
/* ------------------------------------------------------------------ */
static void uart_telemetry(void)
{
    uart_puts("$STS,up=");    uart_putu(g_dbg.uptime_sec);
    uart_puts(",loop=");      uart_putu(g_dbg.loop_count);
    uart_puts(",err=");       uart_putu(g_dbg.error_total);
    uart_puts(",flags=");     uart_puthex(g_dbg.status_flags);
    uart_puts("\r\n");

    /* AIN channel scan — print all 12 channels */
    uart_puts("$AIN");
    for (int i = 0; i < 12; i++) {
        uart_putc(i == 0 ? ',' : ',');
        uart_puti(g_ain[i]);
    }
    uart_puts("\r\n");

    uart_puts("$ADC0,ch=");   uart_putu(g_dbg.adc0_chan);
    uart_puts(",raw=");       uart_puti(g_dbg.adc0_raw);
    uart_puts(",min=");       uart_puti(g_dbg.adc0_min);
    uart_puts(",max=");       uart_puti(g_dbg.adc0_max);
    uart_puts(",rate=");      uart_putu(g_dbg.adc0_rate);
    uart_puts("/s,scans=");   uart_putu(g_dbg.adc0_scans);
    uart_puts("\r\n");

    uart_puts("$ADC1,raw=");  uart_puti(g_dbg.adc1_raw);
    uart_puts(",rate=");      uart_putu(g_dbg.adc1_rate);
    uart_puts("/s\r\n");

    uart_puts("$GPIO,P0out="); uart_puthex(g_dbg.gpio_p0_out);
    uart_puts(",P1in=");       uart_puthex(g_dbg.gpio_p1_in);
    uart_puts(",P2in=");       uart_puthex(g_dbg.gpio_p2_in);
    uart_puts("\r\n");

    uart_puts("$WDT,pets=");  uart_putu(g_dbg.wdt_pets);
    uart_puts(",uart_tx=");   uart_putu(g_dbg.uart_tx_count);
    uart_puts("\r\n---\r\n");
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
void _start(void);

int main(void)
{
    /* Zero debug struct and AIN array */
    volatile uint32_t *p = (volatile uint32_t *)&g_dbg;
    for (int i = 0; i < (int)(sizeof(DbgStruct)/4); i++) p[i] = 0;
    for (int i = 0; i < 12; i++) g_ain[i] = 0;
    g_uart_rx_count  = 0;
    g_uart_rx_errors = 0;

    g_dbg.magic      = 0xDEAD360F;
    g_dbg.fw_version = 0x00020000;  /* v2.0.0 */

    /* Init order: clocks → gpio → uart (can log) → spi1 → adcs → dac → timer → cal → wdt */
    init_clocks();
    init_gpio();
    init_uart();
    init_spi1();
    init_adc0();
    init_adc1();
    init_dac();
    init_timer0();
    cal_read();
    init_wdt();

    uart_puts("$INIT,STABILIZING...\r\n");
    for (volatile int i = 0; i < 500000; i++);

    g_dbg.status_flags |= STS_BOOT_COMPLETE;
    uart_puts("$INIT,COMPLETE\r\n");
    uart_puts("$INIT,g_ain_12ch_after_g_dbg\r\n");
    uart_puts("$INIT,READY\r\n");
    uart_puts("========================================\r\n");

    uint32_t last_tel_sec = 0;
    uint32_t dac_throttle = 0;

    while (1) {
        g_dbg.loop_count++;

        /* Pet watchdog */
        WdtClrInt();
        g_dbg.wdt_pets++;

        /* DAC sawtooth */
        if (++dac_throttle >= 100) {
            dac_throttle = 0;
            g_dbg.dac_step++;
            g_dbg.dac_value = g_dbg.dac_step & 0xFFF;
            DacWr(0, (int)(g_dbg.dac_value << 16));
        }

        process_swd_commands();

        /* UART RX echo */
        if (UrtLinSta(pADI_UART) & COMLSR_DR) {
            char rx = UrtRx(pADI_UART);
            uart_putc(rx);
            g_uart_rx_count++;
        }

        /* Telemetry once per second */
        if (g_dbg.uptime_sec > last_tel_sec) {
            last_tel_sec = g_dbg.uptime_sec;
            uart_telemetry();
        }

        __WFI();
    }
    return 0;
}

void _start(void) { main(); }
