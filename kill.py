#!/usr/bin/env python3
"""Halt the ADuCM360 CPU via SWD."""
from pyocd.core.helpers import ConnectHelper

try:
    session = ConnectHelper.session_with_chosen_probe(
        target_override='aducm360', connect_mode='attach')
    session.open()
    session.target.halt()
    print('CPU halted.')
    session.close()
except Exception as e:
    print(f'Failed: {e}')
    print('Is the DEBUG USB connected?')
