# ADuCM360 Noise/EMF Sensor

24-bit sigma-delta ADC noise sensor using the EVAL-ADICUP360 board.
Reads AIN0 (antenna wire) and displays a real-time scrolling waveform.

## Hardware

- **Board:** EVAL-ADICUP360 Rev 1.1 (Analog Devices)
- **MCU:** ADuCM360 — ARM Cortex-M3, dual 24-bit sigma-delta ADC
- **Debug:** DAPLink (mbed CMSIS-DAP) on DEBUG USB → COM6
- **FTDI:** FT232RL on USER USB → COM5 (UART routing via DIP switches — see below)
- **Antenna:** Wire connected to AIN0 (left header, below DGND)

## Quick Start

### 1. Connect USB cables
- DEBUG USB (left connector) — used for flashing/SWD debug
- USER USB (right connector) — FTDI serial (if UART routing is configured)

### 2. Flash firmware
```bash
# Build (from WSL2)
CMSIS=~/EVAL-ADICUP360/projects/ADuCM360_demo_cn0359_reva/src/system/include/cmsis
LD=~/EVAL-ADICUP360/projects/ADuCM360_demo_cn0359_reva/ld_script/gcc_arm.ld
STARTUP=~/EVAL-ADICUP360/projects/ADuCM360_demo_cn0359_reva/src/system/cmsis
LIBS=~/EVAL-ADICUP360/projects/ADuCM360_demo_blink/IAR/system
SRC=/mnt/c/Users/bob43/Downloads/aducm360_noise_sensor

arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -O1 -std=c99 -DADUCM360 \
  -I$CMSIS -I$LIBS/include/ADuCM360 -T$LD \
  -nostartfiles -nodefaultlibs -Wl,--gc-sections \
  $STARTUP/system_ADuCM360.c $STARTUP/startup_ADuCM360.S \
  $LIBS/src/ADuCM360/AdcLib.c $LIBS/src/ADuCM360/DioLib.c \
  $LIBS/src/ADuCM360/DmaLib.c \
  $SRC/adc_swd.c \
  -o $SRC/adc_swd.elf

arm-none-eabi-objcopy -O binary $SRC/adc_swd.elf $SRC/adc_swd.bin
```

```python
# Flash (from Windows, using pyocd with ADuCM360 pack installed)
from pyocd.core.helpers import ConnectHelper
from pyocd.flash.file_programmer import FileProgrammer

session = ConnectHelper.session_with_chosen_probe(target_override='aducm360', connect_mode='halt')
session.open()
session.target.halt()
FileProgrammer(session, no_reset=True).program('adc_swd.bin', base_address=0x00000000)
session.target.reset()
session.close()
```

### 3. Run viewer
```
python viewer_swd.py
```

## Firmware Files

| File | Purpose |
|------|---------|
| `adc_swd.c` | **WORKING** — ADC on AIN0, output via SWD global variables, LED blink on P0.4 |
| `main.c` | UART version (not working due to switch routing issue) |
| `viewer_swd.py` | Real-time waveform viewer reading ADC via SWD/pyocd |
| `viewer.py` | UART-based viewer (for when UART is fixed) |
| `blink_test.c` | LED blink on P0.4/P0.5 — used to verify firmware is running |
| `bitbang_test.c` | Bit-bang 9600 baud serial on P0.7 — used to test pin routing |
| `uart_both.c` | UART on both P0.1/P0.2 and P0.6/P0.7 — switch routing test |
| `uart_debug.c` | UART on P0.6/P0.7 with LED blink — debug version |

## SWD Global Variables (adc_swd.elf)

| Variable | Address | Description |
|----------|---------|-------------|
| `g_adc_val` | `0x2000000C` | Latest 24-bit ADC reading (sign-extended to int32) |
| `g_magic` | `0x20000010` | Set to `0xCAFE1234` when firmware is running |
| `g_sample_count` | `0x20000014` | Increments with each ADC sample |

**Important:** These addresses change if the firmware is recompiled. After rebuild, run:
```
arm-none-eabi-nm adc_swd.elf | grep -E "g_adc|g_magic|g_sample"
```
Then update the addresses in `viewer_swd.py`.
