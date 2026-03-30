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

    # Verify if it's the SWD firmware
    if 'adc_swd' in bin_path:
        import time
        time.sleep(2)
        try:
            session2 = ConnectHelper.session_with_chosen_probe(
                target_override='cortex_m', connect_mode='attach')
            session2.open()
            magic = session2.target.read32(0x20000010)
            if magic == 0xCAFE1234:
                print(f'Verified: firmware running (magic=0xCAFE1234)')
            else:
                print(f'Warning: magic=0x{magic:08X} (expected 0xCAFE1234)')
                print('  Addresses may have changed — recheck with:')
                print('  arm-none-eabi-nm adc_swd.elf | grep g_magic')
            session2.close()
        except Exception as e:
            print(f'Verify skipped: {e}')

if __name__ == '__main__':
    bin_file = sys.argv[1] if len(sys.argv) > 1 else 'adc_swd.bin'
    # Resolve relative to script directory
    if not os.path.isabs(bin_file):
        bin_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), bin_file)
    flash(bin_file)
