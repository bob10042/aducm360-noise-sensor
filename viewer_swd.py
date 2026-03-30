# -*- coding: utf-8 -*-
"""ADuCM360 Noise Sensor Viewer - reads ADC via SWD debug port"""
import tkinter as tk
import threading
import collections
import time

HISTORY_LEN = 300
UPDATE_MS   = 50
BG          = '#0d1117'
PANEL       = '#161b22'
TEXT        = '#c9d1d9'
GREEN       = '#3fb950'
AMBER       = '#e3b341'

# Memory addresses from adc_swd.elf
G_ADC_VAL   = 0x2000000C
G_MAGIC     = 0x20000010
G_COUNT     = 0x20000014
MAGIC_EXPECT = 0xCAFE1234

class Viewer:
    def __init__(self, root):
        self.root = root
        root.title('ADuCM360 24-bit Noise Sensor (SWD)')
        root.configure(bg=BG)
        root.geometry('900x400')

        self.history = collections.deque([0]*HISTORY_LEN, maxlen=HISTORY_LEN)
        self.session = None
        self.target = None
        self.running = False

        # Top bar
        bar = tk.Frame(root, bg=PANEL, height=36)
        bar.pack(fill='x')
        self.lbl_port = tk.Label(bar, text='Connecting SWD...', bg=PANEL, fg=AMBER,
                                 font=('Consolas', 11))
        self.lbl_port.pack(side='left', padx=10)
        self.lbl_val = tk.Label(bar, text='---', bg=PANEL, fg=GREEN,
                                font=('Consolas', 14, 'bold'))
        self.lbl_val.pack(side='right', padx=10)

        # Canvas
        self.canvas = tk.Canvas(root, bg=BG, highlightthickness=0)
        self.canvas.pack(fill='both', expand=True)

        self._connect()
        self._draw()

    def _connect(self):
        try:
            from pyocd.core.helpers import ConnectHelper
            self.session = ConnectHelper.session_with_chosen_probe(
                target_override='cortex_m', connect_mode='attach')
            self.session.open()
            self.target = self.session.target

            magic = self.target.read32(G_MAGIC)
            if magic == MAGIC_EXPECT:
                self.lbl_port.config(text='Connected via SWD - ADC running', fg=GREEN)
                self.running = True
                threading.Thread(target=self._reader, daemon=True).start()
                return
            else:
                self.lbl_port.config(
                    text=f'Magic mismatch: 0x{magic:08X} - flash adc_swd firmware',
                    fg='#f85149')
        except Exception as e:
            self.lbl_port.config(text=f'SWD: {e}', fg='#f85149')
        self.root.after(3000, self._connect)

    def _reader(self):
        while self.running:
            try:
                raw32 = self.target.read32(G_ADC_VAL)
                raw24 = raw32 & 0xFFFFFF
                if raw24 & 0x800000:
                    raw24 -= 0x1000000
                self.history.append(raw24)
                self.root.after(0, self.lbl_val.config, {'text': str(raw24)})
                time.sleep(0.04)
            except:
                self.running = False
                self.root.after(0, self.lbl_port.config,
                                {'text': 'SWD disconnected', 'fg': AMBER})

    def _draw(self):
        c = self.canvas
        c.delete('all')
        w = c.winfo_width() or 900
        h = c.winfo_height() or 350

        data = list(self.history)
        if max(data) != min(data):
            mn, mx = min(data), max(data)
            pad = max((mx - mn) * 0.1, 1)
            mn -= pad; mx += pad
        else:
            mn, mx = -100, 100

        def y(v): return h - int((v - mn) / (mx - mn) * h)

        # Grid lines
        for i in range(1, 4):
            yg = int(h * i / 4)
            c.create_line(0, yg, w, yg, fill='#21262d')
        c.create_line(0, h//2, w, h//2, fill='#30363d')

        # Waveform
        step = w / max(len(data)-1, 1)
        pts = [(int(i*step), y(v)) for i, v in enumerate(data)]
        if len(pts) > 1:
            c.create_line(*[coord for pt in pts for coord in pt],
                          fill=GREEN, width=1, smooth=False)

        # Scale labels
        c.create_text(4, 4,    text=f'{mx:.0f}', anchor='nw', fill=TEXT,
                      font=('Consolas', 9))
        c.create_text(4, h-14, text=f'{mn:.0f}', anchor='nw', fill=TEXT,
                      font=('Consolas', 9))

        self.root.after(UPDATE_MS, self._draw)

root = tk.Tk()
app = Viewer(root)
root.mainloop()
