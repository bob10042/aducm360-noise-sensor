/* ADuCM360 Full Firmware Stack
 * EVAL-ADICUP360 Rev 1.1 — exercises all major peripherals
 *
 * Peripherals active:
 *   ADC0  — AIN0 (antenna/noise) continuous, interrupt-driven
 *   ADC1  — Internal temp sensor, continuous, interrupt-driven
 *   DAC   — 12-bit sawtooth waveform output on DACOUT pin
 *   Timer0 — 1ms tick for heartbeat, uptime, sample rate calc
 *   WDT   — ~10s watchdog, reset mode
 *   UART  — 115200 on P0.6/P0.7 (verbose telemetry, timeout-guarded)
 *   GPIO  — P0.4 heartbeat LED (1Hz), P0.5 ADC activity LED
 *   Flash — Calibration storage at page 0x1F800
 *
 * Primary output: SWD debug struct (g_dbg) readable via pyocd
 * Secondary output: UART "$" delimited messages (when switches routed)
 *
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

/* ------------------------------------------------------------------ */
/*  SWD Debug Struct — read as one block by viewer_swd_full.py        */
/*  Total: 128 bytes (32 x uint32_t words)                           */
/*  Address: found via  arm-none-eabi-nm firmware_full.elf | grep g_dbg */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed, aligned(4))) {
    /* Header (16 bytes) */
    uint32_t magic;              /* 0x00: 0xDEAD360F when running */
    uint32_t fw_version;         /* 0x04: 0x00010000 = v1.0.0 */
    uint32_t uptime_sec;         /* 0x08: seconds since boot */
    uint32_t status_flags;       /* 0x0C: bitfield — see below */

    /* ADC0 block — antenna/noise (24 bytes) */
    int32_t  adc0_raw;           /* 0x10: latest 24-bit reading (sign-extended) */
    int32_t  adc0_min;           /* 0x14: minimum since boot */
    int32_t  adc0_max;           /* 0x18: maximum since boot */
    uint32_t adc0_count;         /* 0x1C: total samples taken */
    uint32_t adc0_rate;          /* 0x20: samples/sec (computed each second) */
    uint32_t adc0_errors;        /* 0x24: ADC0 error count */

    /* ADC1 block — temperature (24 bytes) */
    int32_t  adc1_raw;           /* 0x28: latest temp reading */
    int32_t  adc1_min;           /* 0x2C: minimum since boot */
    int32_t  adc1_max;           /* 0x30: maximum since boot */
    uint32_t adc1_count;         /* 0x34: total samples taken */
    uint32_t adc1_rate;          /* 0x38: samples/sec */
    uint32_t adc1_errors;        /* 0x3C: ADC1 error count */

    /* DAC block (8 bytes) */
    uint32_t dac_value;          /* 0x40: current 12-bit DAC output */
    uint32_t dac_step;           /* 0x44: sawtooth step counter */

    /* System block (24 bytes) */
    uint32_t tick_ms;            /* 0x48: millisecond tick counter */
    uint32_t timer_overflows;    /* 0x4C: Timer0 overflow count */
    uint32_t wdt_pets;           /* 0x50: watchdog pet count */
    uint32_t gpio_p0_out;        /* 0x54: current P0 output state */
    uint32_t loop_count;         /* 0x58: main loop iterations */
    uint32_t uart_tx_count;      /* 0x5C: UART chars transmitted */

    /* Flash calibration block (8 bytes) */
    uint32_t cal_valid;          /* 0x60: 0xCA1B0000 if cal data valid */
    int32_t  cal_adc0_offset;    /* 0x64: ADC0 offset correction */

    /* Error/command block (8 bytes) */
    uint32_t error_total;        /* 0x68: total errors across all peripherals */
    uint32_t swd_command;        /* 0x6C: write non-zero via SWD to trigger action */
                                 /*   0x01 = save cal to flash                     */
                                 /*   0x02 = zero ADC min/max                      */
                                 /*   0x03 = reset DAC to 0                        */
                                 /*   0xFF = software reset                        */

    /* Verbose telemetry (16 bytes) */
    uint32_t adc0_last8[4];      /* 0x70-0x7C: ring of last 4 ADC0 readings */
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

/* ------------------------------------------------------------------ */
/*  Globals                                                           */
/* ------------------------------------------------------------------ */
volatile DbgStruct g_dbg;

/* ISR-shared temporaries */
static volatile uint32_t s_adc0_count_prev;
static volatile uint32_t s_adc1_count_prev;
static volatile uint8_t  s_adc0_ring_idx;

/* Flash calibration page address (page-aligned, away from FA keys) */
#define CAL_FLASH_ADDR  0x0001F800
#define CAL_MAGIC       0xCA1B0000

/* UART RX counter for loopback testing — readable via SWD */
volatile uint32_t g_uart_rx_count = 0;
volatile uint32_t g_uart_rx_errors = 0;

/* ------------------------------------------------------------------ */
/*  UART helpers (timeout-guarded to survive broken switch routing)   */
/* ------------------------------------------------------------------ */
static void uart_putc(char c)
{
    volatile int timeout = 5000;
    while (!(UrtLinSta(pADI_UART) & COMLSR_THRE) && --timeout > 0);
    if (timeout > 0) {
        UrtTx(pADI_UART, c);
        g_dbg.uart_tx_count++;
    }
}

static void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

static void uart_puti(int32_t v)
{
    char buf[12];
    int i = 0;
    if (v < 0) { uart_putc('-'); v = -v; }
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

static void uart_putu(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

static void uart_puthex(uint32_t v)
{
    const char hex[] = "0123456789ABCDEF";
    uart_putc('0'); uart_putc('x');
    for (int i = 28; i >= 0; i -= 4)
        uart_putc(hex[(v >> i) & 0xF]);
}

/* ------------------------------------------------------------------ */
/*  Flash calibration read/write                                      */
/* ------------------------------------------------------------------ */
static void cal_read(void)
{
    uint32_t *p = (uint32_t *)CAL_FLASH_ADDR;
    if (p[0] == CAL_MAGIC) {
        g_dbg.cal_valid = CAL_MAGIC;
        g_dbg.cal_adc0_offset = (int32_t)p[1];
        g_dbg.status_flags |= STS_CAL_VALID;
        uart_puts("$CAL,LOADED,offset=");
        uart_puti(g_dbg.cal_adc0_offset);
        uart_puts("\r\n");
    } else {
        g_dbg.cal_valid = 0;
        g_dbg.cal_adc0_offset = 0;
        uart_puts("$CAL,NONE\r\n");
    }
}

static void cal_write(void)
{
    g_dbg.status_flags |= STS_FLASH_BUSY;
    uart_puts("$CAL,SAVING...\r\n");

    /* Erase the calibration page */
    FeeWrEn(1);
    FeePErs(CAL_FLASH_ADDR);
    while (FeeSta() & 1);  /* wait for completion */

    /* Write magic + offset (2 words = 8 bytes) */
    uint32_t *dst = (uint32_t *)CAL_FLASH_ADDR;
    dst[0] = CAL_MAGIC;
    while (FeeSta() & 1);
    dst[1] = (uint32_t)g_dbg.cal_adc0_offset;
    while (FeeSta() & 1);

    FeeWrEn(0);
    g_dbg.status_flags &= ~STS_FLASH_BUSY;

    /* Verify */
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
/*  Peripheral init functions                                         */
/* ------------------------------------------------------------------ */
static void init_clocks(void)
{
    /* Enable all peripheral clocks */
    pADI_CLKCTL->CLKDIS = 0x0000;
    uart_puts("$INIT,CLOCKS,ALL_ENABLED\r\n");
}

static void init_gpio(void)
{
    /* P0.4 = LED heartbeat (GPIO output) */
    DioCfgPin(pADI_GP0, 4, 0);
    DioOenPin(pADI_GP0, 4, 1);
    pADI_GP0->GPSET = (1 << 4);

    /* P0.5 = LED ADC activity (GPIO output) */
    DioCfgPin(pADI_GP0, 5, 0);
    DioOenPin(pADI_GP0, 5, 1);
    pADI_GP0->GPCLR = (1 << 5);

    uart_puts("$INIT,GPIO,P0.4=LED_HB,P0.5=LED_ADC\r\n");
}

static void init_uart(void)
{
    /* UART on P0.6(RX func1) / P0.7(TX func2)
     * DioCfg writes entire GPCON register:
     *   P0.7 = func2 (bits 15:14 = 10) = 0x8000
     *   P0.6 = func1 (bits 13:12 = 01) = 0x1000
     *   P0.4 = GPIO  (bits  9:8  = 00) — already set above
     *   P0.5 = GPIO  (bits 11:10 = 00) — already set above
     *   Total = 0x9000
     */
    DioCfg(pADI_GP0, 0x9000);
    UrtCfg(pADI_UART, B115200, COMLCR_WLS_8BITS, 0);

    g_dbg.status_flags |= STS_UART_OK;
    uart_puts("\r\n");
    uart_puts("========================================\r\n");
    uart_puts("  ADuCM360 Full Firmware v1.0.0\r\n");
    uart_puts("  EVAL-ADICUP360 Rev 1.1\r\n");
    uart_puts("========================================\r\n");
    uart_puts("$BOOT,ADuCM360,FW_FULL,v1.0.0\r\n");
    uart_puts("$INIT,UART,115200,8N1,P0.6_RX,P0.7_TX\r\n");
}

static void init_adc0(void)
{
    /* ADC0: AIN0 (antenna) vs AGND, internal 1.2V ref, gain=1
     * Bipolar mode, sinc4 filter SF=31, continuous, interrupt-driven */
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_IDLE);
    AdcRng(pADI_ADC0, ADCCON_ADCREF_INTREF, ADCMDE_PGA_G1, ADCCON_ADCCODE_INT);
    AdcPin(pADI_ADC0, ADCCON_ADCCN_AGND, ADCCON_ADCCP_AIN0);
    AdcFlt(pADI_ADC0, 31, 0, FLT_NORMAL);

    /* Enable ADC0 ready interrupt */
    AdcMski(pADI_ADC0, ADCMSKI_RDY, 1);
    NVIC_EnableIRQ(ADC0_IRQn);

    /* Start continuous conversion */
    AdcGo(pADI_ADC0, ADCMDE_ADCMD_CONT);

    g_dbg.adc0_min = 0x7FFFFF;   /* start at max for min tracking */
    g_dbg.adc0_max = -0x800000;  /* start at min for max tracking */
    g_dbg.status_flags |= STS_ADC0_RUNNING;

    uart_puts("$INIT,ADC0,AIN0_vs_AGND,1.2Vref,G1,SF31,CONT,IRQ\r\n");
}

static void init_adc1(void)
{
    /* ADC1: Internal temperature sensor, slower filter SF=127 */
    AdcGo(pADI_ADC1, ADCMDE_ADCMD_IDLE);
    AdcRng(pADI_ADC1, ADCCON_ADCREF_INTREF, ADCMDE_PGA_G1, ADCCON_ADCCODE_INT);
    AdcPin(pADI_ADC1, ADCCON_ADCCN_AGND, ADCCON_ADCCP_TEMP);
    AdcFlt(pADI_ADC1, 127, 0, FLT_NORMAL);

    /* Enable ADC1 ready interrupt */
    AdcMski(pADI_ADC1, ADCMSKI_RDY, 1);
    NVIC_EnableIRQ(ADC1_IRQn);

    /* Start continuous conversion */
    AdcGo(pADI_ADC1, ADCMDE_ADCMD_CONT);

    g_dbg.adc1_min = 0x7FFFFF;
    g_dbg.adc1_max = -0x800000;
    g_dbg.status_flags |= STS_ADC1_RUNNING;

    uart_puts("$INIT,ADC1,TEMP_SENSOR,1.2Vref,G1,SF127,CONT,IRQ\r\n");
}

static void init_dac(void)
{
    /* 12-bit DAC, internal VREF, normal mode */
    DacCfg(DACCON_CLR_Off, DACCON_RNG_IntVref, DACCON_CLK_HCLK, DACCON_MDE_12bit);
    DacWr(0, 0);

    g_dbg.dac_value = 0;
    g_dbg.dac_step = 0;
    g_dbg.status_flags |= STS_DAC_RUNNING;

    uart_puts("$INIT,DAC,12bit,IntVref,DACOUT_pin\r\n");
}

static void init_timer0(void)
{
    /* Timer0: 1ms tick using UCLK (16MHz) with /256 prescaler
     * 16MHz / 256 = 62500 Hz
     * Load = 62 => period ~ 62/62500 = 0.992ms (close enough to 1ms)
     * Using periodic mode with interrupt
     */
    GptLd(pADI_TM0, 62);
    GptCfg(pADI_TM0, TCON_CLK_UCLK, TCON_PRE_DIV256,
           TCON_MOD_PERIODIC | TCON_UP_DIS | TCON_RLD_EN | TCON_ENABLE_EN);

    NVIC_EnableIRQ(TIMER0_IRQn);

    g_dbg.status_flags |= STS_TIMER_RUNNING;

    uart_puts("$INIT,TIMER0,1ms_tick,UCLK/256,LD=62,IRQ\r\n");
}

static void init_wdt(void)
{
    /* Watchdog: ~10s timeout
     * 32kHz LFOSC / prescaler 16 = 2048 Hz
     * Load 20480 => ~10 second timeout
     * Reset mode (not interrupt)
     */
    WdtCfg(T3CON_PRE_DIV16, T3CON_IRQ_DIS, T3CON_PD_DIS);
    WdtLd(20480);
    WdtGo(T3CON_ENABLE_EN);

    g_dbg.status_flags |= STS_WDT_ACTIVE;

    uart_puts("$INIT,WDT,10s_timeout,RESET_MODE\r\n");
}

/* ------------------------------------------------------------------ */
/*  Interrupt Service Routines                                        */
/*  These override weak symbols from startup_ADuCM360.S               */
/* ------------------------------------------------------------------ */

/* ADC0 ISR — antenna/noise channel */
void ADC0_Int_Handler(void)
{
    int32_t raw = (int32_t)AdcRd(pADI_ADC0);
    /* Sign-extend 24-bit to 32-bit */
    if (raw & 0x800000) raw |= (int32_t)0xFF000000;

    /* Apply calibration offset if valid */
    if (g_dbg.cal_valid == CAL_MAGIC)
        raw -= g_dbg.cal_adc0_offset;

    g_dbg.adc0_raw = raw;
    g_dbg.adc0_count++;

    /* Track min/max */
    if (raw < g_dbg.adc0_min) g_dbg.adc0_min = raw;
    if (raw > g_dbg.adc0_max) g_dbg.adc0_max = raw;

    /* Ring buffer of last 4 readings */
    g_dbg.adc0_last8[s_adc0_ring_idx & 3] = (uint32_t)raw;
    s_adc0_ring_idx++;

    /* Toggle ADC activity LED every 16 samples */
    if (g_dbg.adc0_count & 16)
        pADI_GP0->GPSET = (1 << 5);
    else
        pADI_GP0->GPCLR = (1 << 5);
}

/* ADC1 ISR — temperature channel */
void ADC1_Int_Handler(void)
{
    int32_t raw = (int32_t)AdcRd(pADI_ADC1);
    if (raw & 0x800000) raw |= (int32_t)0xFF000000;

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

    /* Once per second: compute sample rates, toggle heartbeat */
    if ((g_dbg.tick_ms % 1000) == 0) {
        g_dbg.uptime_sec++;

        /* ADC0 sample rate */
        g_dbg.adc0_rate = g_dbg.adc0_count - s_adc0_count_prev;
        s_adc0_count_prev = g_dbg.adc0_count;

        /* ADC1 sample rate */
        g_dbg.adc1_rate = g_dbg.adc1_count - s_adc1_count_prev;
        s_adc1_count_prev = g_dbg.adc1_count;

        /* Heartbeat LED toggle on P0.4 (1Hz blink) */
        if (g_dbg.uptime_sec & 1)
            pADI_GP0->GPSET = (1 << 4);
        else
            pADI_GP0->GPCLR = (1 << 4);

        /* Snapshot GPIO state */
        g_dbg.gpio_p0_out = pADI_GP0->GPOUT;
    }
}

/* ------------------------------------------------------------------ */
/*  SWD Command Processor                                             */
/*  Write to g_dbg.swd_command via pyocd to trigger actions           */
/* ------------------------------------------------------------------ */
static void process_swd_commands(void)
{
    uint32_t cmd = g_dbg.swd_command;
    if (cmd == 0) return;

    g_dbg.swd_command = 0;  /* acknowledge */

    switch (cmd) {
    case 0x01:  /* Save calibration to flash */
        uart_puts("$CMD,SAVE_CAL\r\n");
        /* Use current ADC0 reading as offset (for zero calibration) */
        g_dbg.cal_adc0_offset = g_dbg.adc0_raw;
        cal_write();
        break;

    case 0x02:  /* Reset ADC min/max */
        uart_puts("$CMD,RESET_MINMAX\r\n");
        g_dbg.adc0_min = 0x7FFFFF;
        g_dbg.adc0_max = -0x800000;
        g_dbg.adc1_min = 0x7FFFFF;
        g_dbg.adc1_max = -0x800000;
        break;

    case 0x03:  /* Reset DAC to 0 */
        uart_puts("$CMD,RESET_DAC\r\n");
        g_dbg.dac_value = 0;
        g_dbg.dac_step = 0;
        DacWr(0, 0);
        break;

    case 0xFF:  /* Software reset */
        uart_puts("$CMD,RESET!\r\n");
        NVIC_SystemReset();
        break;

    default:
        uart_puts("$CMD,UNKNOWN,");
        uart_puthex(cmd);
        uart_puts("\r\n");
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  UART verbose telemetry output                                     */
/* ------------------------------------------------------------------ */
static void uart_telemetry(void)
{
    /* Full status line every second */
    uart_puts("$STS,up=");
    uart_putu(g_dbg.uptime_sec);
    uart_puts(",tick=");
    uart_putu(g_dbg.tick_ms);
    uart_puts(",loop=");
    uart_putu(g_dbg.loop_count);
    uart_puts(",err=");
    uart_putu(g_dbg.error_total);
    uart_puts(",flags=");
    uart_puthex(g_dbg.status_flags);
    uart_puts("\r\n");

    /* ADC0 detail */
    uart_puts("$ADC0,raw=");
    uart_puti(g_dbg.adc0_raw);
    uart_puts(",min=");
    uart_puti(g_dbg.adc0_min);
    uart_puts(",max=");
    uart_puti(g_dbg.adc0_max);
    uart_puts(",cnt=");
    uart_putu(g_dbg.adc0_count);
    uart_puts(",rate=");
    uart_putu(g_dbg.adc0_rate);
    uart_puts("/s,err=");
    uart_putu(g_dbg.adc0_errors);
    uart_puts("\r\n");

    /* ADC1/temp detail */
    uart_puts("$ADC1,raw=");
    uart_puti(g_dbg.adc1_raw);
    uart_puts(",min=");
    uart_puti(g_dbg.adc1_min);
    uart_puts(",max=");
    uart_puti(g_dbg.adc1_max);
    uart_puts(",cnt=");
    uart_putu(g_dbg.adc1_count);
    uart_puts(",rate=");
    uart_putu(g_dbg.adc1_rate);
    uart_puts("/s\r\n");

    /* DAC detail */
    uart_puts("$DAC,val=");
    uart_putu(g_dbg.dac_value);
    uart_puts(",step=");
    uart_putu(g_dbg.dac_step);
    uart_puts("\r\n");

    /* Calibration */
    if (g_dbg.cal_valid == CAL_MAGIC) {
        uart_puts("$CAL,VALID,offset=");
        uart_puti(g_dbg.cal_adc0_offset);
    } else {
        uart_puts("$CAL,NONE");
    }
    uart_puts("\r\n");

    /* WDT */
    uart_puts("$WDT,pets=");
    uart_putu(g_dbg.wdt_pets);
    uart_puts(",uart_tx=");
    uart_putu(g_dbg.uart_tx_count);
    uart_puts("\r\n");

    uart_puts("---\r\n");
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */
void _start(void);

int main(void)
{
    /* ---- Zero the debug struct (don't rely on .bss init) ---- */
    volatile uint32_t *p = (volatile uint32_t *)&g_dbg;
    for (int i = 0; i < (int)(sizeof(DbgStruct)/4); i++)
        p[i] = 0;

    /* ---- Set identity ---- */
    g_dbg.magic = 0xDEAD360F;
    g_dbg.fw_version = 0x00010000;  /* v1.0.0 */

    /* ---- Init all peripherals (order matters) ---- */
    init_clocks();
    init_uart();      /* UART first so other inits can log */
    init_gpio();
    init_adc0();
    init_adc1();
    init_dac();
    init_timer0();

    /* Read calibration from flash before starting WDT */
    cal_read();

    /* Start watchdog last (everything else must be ready) */
    init_wdt();

    /* ---- Stabilization delay ---- */
    uart_puts("$INIT,STABILIZING...\r\n");
    for (volatile int i = 0; i < 500000; i++);

    g_dbg.status_flags |= STS_BOOT_COMPLETE;
    uart_puts("$INIT,COMPLETE,ALL_PERIPHERALS_ACTIVE\r\n");
    uart_puts("$INIT,SWD_STRUCT_SIZE=128_bytes\r\n");
    uart_puts("$INIT,SWD_MAGIC=0xDEAD360F\r\n");
    uart_puts("$INIT,READY\r\n");
    uart_puts("========================================\r\n");

    /* ---- Variables for main loop throttling ---- */
    uint32_t last_telemetry_sec = 0;
    uint32_t dac_throttle = 0;

    /* ---- Main loop ---- */
    while (1) {
        g_dbg.loop_count++;

        /* Pet the watchdog */
        WdtClrInt();
        g_dbg.wdt_pets++;

        /* Step DAC sawtooth (0 → 4095 → 0 → ...) every ~100 loops */
        dac_throttle++;
        if (dac_throttle >= 100) {
            dac_throttle = 0;
            g_dbg.dac_step++;
            g_dbg.dac_value = (g_dbg.dac_step & 0xFFF);
            /* DACDAT register: value in bits [31:16] for 12-bit mode */
            DacWr(0, (int)(g_dbg.dac_value << 16));
        }

        /* Process SWD commands (viewer can write to g_dbg.swd_command) */
        process_swd_commands();

        /* UART RX echo — any received byte gets sent back */
        if (UrtLinSta(pADI_UART) & COMLSR_DR) {
            char rx = UrtRx(pADI_UART);
            uart_putc(rx);
            g_uart_rx_count++;
        }

        /* Verbose UART telemetry once per second */
        if (g_dbg.uptime_sec > last_telemetry_sec) {
            last_telemetry_sec = g_dbg.uptime_sec;
            uart_telemetry();
        }

        /* Sleep until next interrupt (saves power, reduces loop rate) */
        __WFI();
    }

    return 0;
}

void _start(void) { main(); }
