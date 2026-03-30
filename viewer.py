# -*- coding: utf-8 -*-
"""ADuCM360 Noise Sensor Viewer - real-time scrolling waveform"""
import tkinter as tk
import serial
import serial.tools.list_ports
import threading
import collections
import time

BAUD        = 115200
HISTORY_LEN = 300
UPDATE_MS   = 50
BG          = '#0d1117'
PANEL       = '#161b22'
TEXT        = '#c9d1d9'
GREEN       = '#3fb950'
AMBER       = '#e3b341'

def find_ftdi():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x0403:
            return p.device
    return None

class Viewer:
    def __init__(self, root):
        self.root = root
        root.title('ADuCM360 Noise Sensor')
        root.configure(bg=BG)
        root.geometry('900x400')

        self.history = collections.deque([0]*HISTORY_LEN, maxlen=HISTORY_LEN)
        self.port    = None
        self.running = False

        # Top bar
        bar = tk.Frame(root, bg=PANEL, height=36)
        bar.pack(fill='x')
        self.lbl_port = tk.Label(bar, text='Searching...', bg=PANEL, fg=AMBER,
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
        port = find_ftdi()
        if port:
            try:
                self.port = serial.Serial(port, BAUD, timeout=0.1)
                self.lbl_port.config(text=f'Connected: {port}', fg=GREEN)
                self.running = True
                threading.Thread(target=self._reader, daemon=True).start()
                return
            except Exception as e:
                self.lbl_port.config(text=f'Error: {e}', fg='#f85149')
        else:
            self.lbl_port.config(text='FTDI not found - plug in UART cable', fg=AMBER)
        self.root.after(2000, self._connect)

    def _reader(self):
        while self.running:
            try:
                line = self.port.readline().decode('ascii', errors='ignore').strip()
                if line.startswith('$ADC,'):
                    val = int(line.split(',')[1])
                    self.history.append(val)
                    self.root.after(0, self.lbl_val.config, {'text': str(val)})
            except:
                self.running = False
                self.root.after(0, self.lbl_port.config,
                                {'text': 'Disconnected', 'fg': AMBER})

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
