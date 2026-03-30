#!/usr/bin/env python3
"""Flash ADuCM360 firmware via pyocd.

Usage: python flash.py [firmware.bin]
Default: adc_swd.bin

IMPORTANT: Before flashing, do the reset sequence:
1. Unplug DEBUG USB cable
2. Hold RESET button on board
3. Plug DEBUG USB back in (while holding reset)
4. Release RESET after 3 seconds
5. Wait for MBED drive to appear
6. Then run this script
"""
import sys
import os

def flash(bin_path):
    from pyocd.core.helpers import ConnectHelper
    from pyocd.flash.file_programmer import FileProgrammer

    if not os.path.exists(bin_path):
        print(f'File not found: {bin_path}')
        sys.exit(1)

    size = os.path.getsize(bin_path)
    print(f'Flashing {bin_path} ({size} bytes)...')

    try:
        session = ConnectHelper.session_with_chosen_probe(
            target_override='aducm360', connect_mode='halt')
        session.open()
        target = session.target
        target.halt()
    except Exception as e:
        print(f'\nERROR: {e}')
        print('\nDid you do the reset sequence?')
        print('  1. Unplug DEBUG USB')
        print('  2. Hold RESET button')
        print('  3. Plug DEBUG USB back in')
        print('  4. Release RESET after 3 seconds')
        sys.exit(1)

    programmer = FileProgrammer(session, no_reset=True)
    programmer.program(bin_path, base_address=0x00000000)
    print('Flash complete')

    target.reset()
    print('Chip running')
    session.close()

    # Verify firmware_full is running
    if 'firmware_full' in bin_path:
        import time, subprocess
        time.sleep(2)
        # Find g_dbg address from ELF
        dbg_addr = 0x2000000C
        elf = bin_path.replace('.bin', '.elf')
        try:
            out = subprocess.check_output(
                ['arm-none-eabi-nm', elf], text=True, stderr=subprocess.DEVNULL)
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[2] == 'g_dbg':
                    dbg_addr = int(parts[0], 16)
        except Exception:
            pass
        try:
            session2 = ConnectHelper.session_with_chosen_probe(
                target_override='cortex_m', connect_mode='attach',
                options={'frequency': 100000})
            session2.open()
            magic = session2.target.read32(dbg_addr)
            if magic == 0xDEAD360F:
                print(f'Verified: firmware_full running (magic=0xDEAD360F @ 0x{dbg_addr:08X})')
            else:
                print(f'Warning: magic=0x{magic:08X} at 0x{dbg_addr:08X} — firmware may still be booting')
            session2.close()
        except Exception as e:
            print(f'Verify skipped: {e}')

if __name__ == '__main__':
    bin_file = sys.argv[1] if len(sys.argv) > 1 else 'adc_swd.bin'
    # Resolve relative to script directory
    if not os.path.isabs(bin_file):
        bin_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), bin_file)
    flash(bin_file)
