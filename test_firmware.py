#!/usr/bin/env python3
"""Automated smoke test for ADuCM360 Full Firmware via SWD.

Tests:
  1. Magic word present (firmware running)
  2. Firmware version matches expected
  3. Uptime incrementing (not stuck)
  4. ADC0 sampling (count increasing, rate > 0)
  5. ADC1 sampling (count increasing, rate > 0)
  6. DAC stepping (value changing)
  7. Timer ticking (tick_ms increasing)
  8. WDT being petted (wdt_pets increasing)
  9. Loop count increasing
  10. Status flags correct (all peripherals active)
  11. No errors accumulated
  12. ADC0 min/max tracking working

Usage:
  python test_firmware.py                # auto-detect g_dbg address from .elf
  python test_firmware.py 0x2000000C     # specify address manually
"""
import sys
import os
import time
import subprocess

MAGIC_EXPECT = 0xDEAD360F
FW_VERSION_EXPECT = 0x00010000
STRUCT_WORDS = 32

# All expected status flags when fully booted
STS_ALL = (
    (1 << 0) |  # ADC0_RUNNING
    (1 << 1) |  # ADC1_RUNNING
    (1 << 2) |  # DAC_RUNNING
    (1 << 3) |  # UART_OK
    (1 << 4) |  # WDT_ACTIVE
    (1 << 5) |  # TIMER_RUNNING
    (1 << 8)    # BOOT_COMPLETE
)

def find_dbg_addr():
    """Try to find g_dbg address from firmware_full.elf."""
    elf = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       'firmware_full.elf')
    if not os.path.exists(elf):
        return None
    try:
        out = subprocess.check_output(
            ['arm-none-eabi-nm', elf], text=True, stderr=subprocess.DEVNULL)
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == 'g_dbg':
                return int(parts[0], 16)
    except Exception:
        pass
    return None

def scan_for_magic(target):
    """Scan first 1KB of RAM for the magic word."""
    print('  Scanning RAM 0x20000000..0x20000400 for magic...')
    for off in range(0, 0x400, 4):
        addr = 0x20000000 + off
        val = target.read32(addr)
        if val == MAGIC_EXPECT:
            print(f'  Found magic at 0x{addr:08X}')
            return addr
    return None

def s32(v):
    """Unsigned 32-bit to signed."""
    return v if v < 0x80000000 else v - 0x100000000

def read_struct(target, addr):
    """Read the full 128-byte debug struct."""
    words = target.read_memory_block32(addr, STRUCT_WORDS)
    return {
        'magic':           words[0],
        'fw_version':      words[1],
        'uptime_sec':      words[2],
        'status_flags':    words[3],
        'adc0_raw':        s32(words[4]),
        'adc0_min':        s32(words[5]),
        'adc0_max':        s32(words[6]),
        'adc0_count':      words[7],
        'adc0_rate':       words[8],
        'adc0_errors':     words[9],
        'adc1_raw':        s32(words[10]),
        'adc1_min':        s32(words[11]),
        'adc1_max':        s32(words[12]),
        'adc1_count':      words[13],
        'adc1_rate':       words[14],
        'adc1_errors':     words[15],
        'dac_value':       words[16],
        'dac_step':        words[17],
        'tick_ms':         words[18],
        'timer_overflows': words[19],
        'wdt_pets':        words[20],
        'gpio_p0_out':     words[21],
        'loop_count':      words[22],
        'uart_tx_count':   words[23],
        'cal_valid':       words[24],
        'cal_adc0_offset': s32(words[25]),
        'error_total':     words[26],
        'swd_command':     words[27],
    }

def main():
    from pyocd.core.helpers import ConnectHelper

    # Determine g_dbg address
    if len(sys.argv) > 1:
        dbg_addr = int(sys.argv[1], 0)
        print(f'Using provided address: 0x{dbg_addr:08X}')
    else:
        dbg_addr = find_dbg_addr()
        if dbg_addr:
            print(f'Found g_dbg in ELF at 0x{dbg_addr:08X}')
        else:
            dbg_addr = None
            print('No ELF found — will scan RAM for magic')

    # Connect
    print('\nConnecting SWD (attach mode)...')
    session = ConnectHelper.session_with_chosen_probe(
        target_override='cortex_m', connect_mode='attach')
    session.open()
    target = session.target

    # Find address if needed
    if dbg_addr is None:
        dbg_addr = scan_for_magic(target)
        if dbg_addr is None:
            print('FAIL: Magic word 0xDEAD360F not found in RAM')
            session.close()
            sys.exit(1)

    print(f'Debug struct at 0x{dbg_addr:08X}\n')

    # ---- Run tests ----
    passed = 0
    failed = 0
    total = 0

    def test(name, condition, detail=''):
        nonlocal passed, failed, total
        total += 1
        if condition:
            passed += 1
            print(f'  PASS  {name}')
        else:
            failed += 1
            print(f'  FAIL  {name}  ({detail})')

    # Snapshot 1
    print('--- Snapshot 1 ---')
    d1 = read_struct(target, dbg_addr)

    test('Magic word',
         d1['magic'] == MAGIC_EXPECT,
         f"got 0x{d1['magic']:08X}")

    test('Firmware version',
         d1['fw_version'] == FW_VERSION_EXPECT,
         f"got 0x{d1['fw_version']:08X}")

    test('Boot complete flag',
         d1['status_flags'] & (1 << 8),
         f"flags=0x{d1['status_flags']:04X}")

    test('ADC0 running flag',
         d1['status_flags'] & (1 << 0),
         f"flags=0x{d1['status_flags']:04X}")

    test('ADC1 running flag',
         d1['status_flags'] & (1 << 1),
         f"flags=0x{d1['status_flags']:04X}")

    test('DAC running flag',
         d1['status_flags'] & (1 << 2),
         f"flags=0x{d1['status_flags']:04X}")

    test('Timer running flag',
         d1['status_flags'] & (1 << 5),
         f"flags=0x{d1['status_flags']:04X}")

    test('WDT active flag',
         d1['status_flags'] & (1 << 4),
         f"flags=0x{d1['status_flags']:04X}")

    test('ADC0 has samples',
         d1['adc0_count'] > 0,
         f"count={d1['adc0_count']}")

    test('ADC1 has samples',
         d1['adc1_count'] > 0,
         f"count={d1['adc1_count']}")

    test('Timer ticking',
         d1['tick_ms'] > 0,
         f"tick_ms={d1['tick_ms']}")

    test('Loops running',
         d1['loop_count'] > 0,
         f"loop_count={d1['loop_count']}")

    test('WDT being petted',
         d1['wdt_pets'] > 0,
         f"wdt_pets={d1['wdt_pets']}")

    test('No errors',
         d1['error_total'] == 0,
         f"errors={d1['error_total']}")

    # Wait 2 seconds, take second snapshot
    print('\n  Waiting 2 seconds for delta check...\n')
    time.sleep(2)

    print('--- Snapshot 2 ---')
    d2 = read_struct(target, dbg_addr)

    test('Uptime incrementing',
         d2['uptime_sec'] > d1['uptime_sec'],
         f"{d1['uptime_sec']} -> {d2['uptime_sec']}")

    test('ADC0 count incrementing',
         d2['adc0_count'] > d1['adc0_count'],
         f"{d1['adc0_count']} -> {d2['adc0_count']}")

    test('ADC1 count incrementing',
         d2['adc1_count'] > d1['adc1_count'],
         f"{d1['adc1_count']} -> {d2['adc1_count']}")

    test('ADC0 rate > 0',
         d2['adc0_rate'] > 0,
         f"rate={d2['adc0_rate']}")

    test('ADC1 rate > 0',
         d2['adc1_rate'] > 0,
         f"rate={d2['adc1_rate']}")

    test('DAC stepping',
         d2['dac_step'] > d1['dac_step'],
         f"{d1['dac_step']} -> {d2['dac_step']}")

    test('Loop count incrementing',
         d2['loop_count'] > d1['loop_count'],
         f"{d1['loop_count']} -> {d2['loop_count']}")

    test('Timer ticks incrementing',
         d2['tick_ms'] > d1['tick_ms'],
         f"{d1['tick_ms']} -> {d2['tick_ms']}")

    test('WDT pets incrementing',
         d2['wdt_pets'] > d1['wdt_pets'],
         f"{d1['wdt_pets']} -> {d2['wdt_pets']}")

    test('Still no errors',
         d2['error_total'] == 0,
         f"errors={d2['error_total']}")

    test('ADC0 min <= raw <= max',
         d2['adc0_min'] <= d2['adc0_raw'] <= d2['adc0_max'],
         f"min={d2['adc0_min']} raw={d2['adc0_raw']} max={d2['adc0_max']}")

    # ---- Summary ----
    print(f'\n{"="*50}')
    print(f'  RESULTS: {passed}/{total} passed, {failed} failed')
    print(f'{"="*50}')

    # Dump full state
    print('\n--- Full Debug Struct Dump ---')
    for k, v in d2.items():
        if isinstance(v, int) and k != 'cal_adc0_offset':
            if k in ('magic', 'fw_version', 'status_flags', 'cal_valid'):
                print(f'  {k:20s} = 0x{v:08X}')
            else:
                print(f'  {k:20s} = {v:>12,}')
        else:
            print(f'  {k:20s} = {v}')

    session.close()

    sys.exit(0 if failed == 0 else 1)

if __name__ == '__main__':
    main()
