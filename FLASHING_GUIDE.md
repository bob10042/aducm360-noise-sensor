# EVAL-ADICUP360 Flashing & Programming Guide

Hard-won lessons from getting the ADuCM360 toolchain working without CrossCore IDE.

## Toolchain (No CrossCore Required)

- **Compiler:** ARM GCC (`arm-none-eabi-gcc` 13.2.1) in WSL2 Ubuntu
- **Flasher:** pyocd (Python, runs on Windows) with ADuCM360 CMSIS pack
- **Debugger:** pyocd for SWD memory reads while chip is running
- **Source headers:** Cloned from `github.com/analogdevicesinc/EVAL-ADICUP360`

### Source file locations in the cloned repo
```
CMSIS headers:   projects/ADuCM360_demo_cn0359_reva/src/system/include/cmsis/
Linker script:   projects/ADuCM360_demo_cn0359_reva/ld_script/gcc_arm.ld
Startup files:   projects/ADuCM360_demo_cn0359_reva/src/system/cmsis/
                   - system_ADuCM360.c (SystemInit — sets 16MHz HFOSC, enables clocks)
                   - startup_ADuCM360.S (vector table, calls SystemInit then _start)
Device headers:  projects/ADuCM360_demo_blink/IAR/system/include/ADuCM360/
Device libs:     projects/ADuCM360_demo_blink/IAR/system/src/ADuCM360/
                   - UrtLib.c, DioLib.c, AdcLib.c, DmaLib.c
```

## Flashing Procedure

### Step 1: Install pyocd and ADuCM360 pack (one-time)
```
pip install pyocd
pyocd pack install ADuCM360
```

### Step 2: Reset sequence (REQUIRED before every flash)
The DAPLink SWD connection becomes unresponsive after each flash+run cycle.
You MUST do this physical reset before every flash:

1. Unplug the DEBUG USB cable from the board
2. Hold the RESET button on the board
3. Plug the DEBUG USB cable back in (while still holding reset)
4. Release the RESET button after 3 seconds
5. Wait for the MBED drive to appear in Windows

### Step 3: Flash via pyocd Python API
```python
from pyocd.core.helpers import ConnectHelper
from pyocd.flash.file_programmer import FileProgrammer

session = ConnectHelper.session_with_chosen_probe(
    target_override='aducm360', connect_mode='halt')
session.open()
session.target.halt()

# no_reset=True prevents pyocd's broken post-flash reset
programmer = FileProgrammer(session, no_reset=True)
programmer.program('firmware.bin', base_address=0x00000000)

session.target.reset()  # Start the firmware
session.close()
```

### Step 4: Verify firmware is running
After flashing, the chip should start executing. If the firmware has an LED blink,
visually confirm. For SWD-based firmware, reconnect and check the magic number:
```python
session = ConnectHelper.session_with_chosen_probe(
    target_override='cortex_m', connect_mode='attach')
session.open()
magic = session.target.read32(0x20000010)  # g_magic address
print(f'Magic: 0x{magic:08X}')  # expect 0xCAFE1234
session.close()
```

## Known Problems & Solutions

### Problem: "No cores were discovered!" from pyocd
**Cause:** DAPLink can't reach the ADuCM360 via SWD after a flash+run cycle.
**Fix:** Do the physical reset sequence (Step 2 above). Every time.

### Problem: pyocd CLI says "Probe not found" but Python API works
**Cause:** Unicode/encoding issue in pyocd CLI with this DAPLink's serial number.
**Fix:** Use the Python API instead of the CLI (`pyocd flash` command).

### Problem: MBED drag-and-drop gives "TIMEOUT" in FAIL.TXT
**Cause:** DAPLink can't halt the chip via SWD before programming.
**Fix:** Use pyocd instead. pyocd halts the chip properly before writing flash.

### Problem: MBED drag-and-drop gives "SWD ERROR" in FAIL.TXT
**Cause:** Another tool (pyocd) still has the SWD interface locked.
**Fix:** Close all pyocd sessions before attempting drag-and-drop.

### Problem: "Device busy" when using usbipd to forward DAPLink to WSL2
**Cause:** USBPcap (Wireshark USB capture driver) holds the USB device.
**Fix:** Stop/disable USBPcap service, or uninstall it entirely:
  `C:\Program Files\USBPcap\Uninstall.exe` (run as administrator)

### Problem: Firmware flashes OK but doesn't appear to run
**Cause:** pyocd's post-flash reset leaves the chip in a bad state.
**Fix:** Use `no_reset=True` when programming, then do `target.reset()` explicitly,
or physically power-cycle the board (unplug all USB, wait 3s, replug).

### Problem: Global variables contain garbage after flash
**Cause:** The startup code may not copy .data section or zero .bss before main().
**Fix:** Don't rely on initialized global variables. Set all globals explicitly
in main() instead of using initializers:
```c
// BAD — may not work:
volatile int32_t g_val = 0xDEAD;

// GOOD — always works:
volatile int32_t g_val;
// ... in main():
g_val = 0xDEAD;
```

### Problem: Firmware hangs when using UART (uart_puts blocks)
**Cause:** If UART switch routing isn't configured, THRE flag may never set.
**Fix:** For ADC-only applications, remove all UART code. Use SWD to read
ADC values directly from global variables. See `adc_swd.c`.

## Build Notes

### Entry point
The startup assembly (`startup_ADuCM360.S`) calls `_start`, not `main`.
Add this to every firmware:
```c
void _start(void);
int main(void) { /* your code */ }
void _start(void) { main(); }
```

### Linker flags
```
-nostartfiles -nodefaultlibs -Wl,--gc-sections
```
- `-nostartfiles` — don't link default crt0 (we provide our own startup_ADuCM360.S)
- `-nodefaultlibs` — no standard library (bare metal)
- `--gc-sections` — remove unused code/data

### GPIO pin functions (ADuCM360)
- `DioCfgPin(port, pin, func)` — safe, read-modify-write on single pin
- `DioCfg(port, value)` — writes ENTIRE GPCON register (all 8 pins at once)
- `DioOenPin(port, pin, enable)` — set output enable for one pin
- For UART: P0.6=func1(RXD), P0.7=func2(TXD) → `DioCfg(pADI_GP0, 0x9000)`
- For UART alt: P0.1=func3(RXD), P0.2=func3(TXD) → `DioCfg(pADI_GP0, 0x003C)`

### ADC configuration
```c
AdcGo(pADI_ADC0, ADCMDE_ADCMD_IDLE);          // Idle before config
AdcRng(pADI_ADC0, ADCCON_ADCREF_INTREF,        // Internal 1.2V ref
       ADCMDE_PGA_G1, ADCCON_ADCCODE_INT);     // Gain=1, bipolar
AdcPin(pADI_ADC0, ADCCON_ADCCN_AGND,           // Negative = AGND
       ADCCON_ADCCP_AIN0);                      // Positive = AIN0
AdcFlt(pADI_ADC0, 31, 0, FLT_NORMAL);          // Filter: SF=31
AdcGo(pADI_ADC0, ADCMDE_ADCMD_CONT);           // Start continuous
```

### Reading ADC result (24-bit sign extension)
```c
int32_t raw = (int32_t)AdcRd(pADI_ADC0);
if (raw & 0x800000) raw |= (int32_t)0xFF000000;  // Sign extend bit 23
```

## Reading ADC via SWD (bypasses UART entirely)
```python
from pyocd.core.helpers import ConnectHelper

session = ConnectHelper.session_with_chosen_probe(
    target_override='cortex_m', connect_mode='attach')
session.open()

raw32 = session.target.read32(0x2000000C)  # g_adc_val
raw24 = raw32 & 0xFFFFFF
if raw24 & 0x800000:
    raw24 -= 0x1000000  # Sign extend

print(f'ADC value: {raw24}')
session.close()
```

## DIP Switch Configuration (UART routing)

The EVAL-ADICUP360 has 4 DIP switches (S1-S4) that route the MCU's UART pins
to different destinations. The truth table is printed on the PCB silkscreen.

**STATUS: UART routing NOT working** — tried multiple switch combinations
without success. The SWD approach bypasses this entirely.

### Switch table from PCB silkscreen:
```
S1  S2  S3  S4  | P01/P02 dest | P06/P07 dest
----|-----|-----|------|--------------|-------------
0   1    1   0  | DEBUG USB    | (check PCB)
1   1    0   0  | USER USB     | (check PCB)
0   0    X   X  | GPIO headers | (check PCB)
```

### What was tried and failed:
- S1=1, S2=1, S3=0, S4=1 → COM5 silent, COM6 silent
- S1=0, S2=1, S3=0, S4=1 → COM5 silent, COM6 silent
- S1=1, S2=1, S3=0, S4=0 → COM5 silent, COM6 silent (from PCB table row 2)
- Header loopback TX↔RX → no echo (header TX/RX not connected to FTDI)

### TODO: Investigate UART routing
- Read the PCB switch table more carefully with magnification
- Try S1=0, S2=1, S3=1, S4=0 (row 1: P01/P02 → DEBUG USB)
- Test sending data TO COM5/COM6 while monitoring MCU GPIO inputs
- Check if the FT232RL chip has a fault (measure TXD/RXD pins with scope)
