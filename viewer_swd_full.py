# -*- coding: utf-8 -*-
"""ADuCM360 Full Firmware Viewer v2.0 — reads debug struct via SWD

Panels (left):
  ADC0 noise waveform (AIN0)
  AIN0-AIN11 live bar chart
  UART loopback bar
  ADC1 temperature waveform

Panels (right):
  ADC0 stats | ADC1 temp | DAC | UART loopback | GPIO | System

Usage: python viewer_swd_full.py
"""
import tkinter as tk
import threading
import collections
import time

# ---- Visual theme ----------------------------------------------------
BG     = '#0d1117'
PANEL  = '#161b22'
PANEL2 = '#1c2333'
TEXT   = '#c9d1d9'
DIM    = '#8b949e'
GREEN  = '#3fb950'
AMBER  = '#e3b341'
RED    = '#f85149'
BLUE   = '#58a6ff'
CYAN   = '#39d353'
PURPLE = '#bc8cff'

FONT   = ('Consolas', 10)
FONT_B = ('Consolas', 10, 'bold')
FONT_L = ('Consolas', 12, 'bold')
FONT_S = ('Consolas', 9)

# ---- Default addresses (overridden from ELF when available) ----------
G_DBG_ADDR_DEFAULT          = 0x2000000C
G_AIN_ADDR_DEFAULT          = 0x20000090   # nm: g_ain
UART_RX_COUNT_ADDR_DEFAULT  = 0x200000C8   # nm: g_uart_rx_count
UART_RX_ERRORS_ADDR_DEFAULT = 0x200000CC   # nm: g_uart_rx_errors

MAGIC_EXPECT = 0xDEAD360F
STRUCT_WORDS = 32

# ---- Temperature conversion (ADuCM360 28-bit ADC, internal ref 1.2V) -
ADC1_FS   = 268435455.0
ADC1_VREF = 1.2
ADC1_V0   = 0.206     # voltage at 0°C
ADC1_TC   = 0.0016    # V/°C

HISTORY_LEN = 400
UPDATE_MS   = 80

STS_ADC0_RUNNING  = 1 << 0
STS_ADC1_RUNNING  = 1 << 1
STS_DAC_RUNNING   = 1 << 2
STS_UART_OK       = 1 << 3
STS_WDT_ACTIVE    = 1 << 4
STS_TIMER_RUNNING = 1 << 5
STS_CAL_VALID     = 1 << 6
STS_BOOT_COMPLETE = 1 << 8
STS_SPI1_READY    = 1 << 9
STS_GPIO_READY    = 1 << 10

AIN_COLORS = [
    AMBER,   # AIN0 — noise antenna (highlighted)
    GREEN, GREEN, GREEN,
    CYAN,  CYAN,  CYAN,
    BLUE,  BLUE,  BLUE,
    PURPLE, PURPLE,
]


def s32(v):
    """uint32 → signed int32."""
    return v if v < 0x80000000 else v - 0x100000000


def raw_to_c(raw):
    """28-bit ADC1 raw → approximate °C."""
    v = s32(raw) / ADC1_FS * ADC1_VREF
    return (v - ADC1_V0) / ADC1_TC


# ======================================================================
class FullViewer:
    def __init__(self, root):
        self.root    = root
        self.session = None
        self.target  = None
        self.running = False
        self.dbg_addr        = G_DBG_ADDR_DEFAULT
        self.ain_addr        = G_AIN_ADDR_DEFAULT
        self.uart_rx_addr    = UART_RX_COUNT_ADDR_DEFAULT
        self.uart_rx_err_addr= UART_RX_ERRORS_ADDR_DEFAULT

        self._lock = threading.Lock()
        self._data = {}

        # Waveform histories
        self.adc0_hist = collections.deque([0] * HISTORY_LEN, maxlen=HISTORY_LEN)
        self.adc1_hist = collections.deque([0] * HISTORY_LEN, maxlen=HISTORY_LEN)
        self.tx_hist   = collections.deque([0] * 60, maxlen=60)
        self.rx_hist   = collections.deque([0] * 60, maxlen=60)

        # AIN snapshot (12 channels, updated each read)
        self.ain_snap  = [0] * 12

        # UART rate tracking
        self._tx_prev   = 0
        self._rx_prev   = 0
        self._tx_rate   = 0
        self._rx_rate   = 0
        self._rate_t    = time.time()
        self._init_uart = False

        root.title('ADuCM360 Firmware Dashboard v2.0 — SWD')
        root.configure(bg=BG)
        root.geometry('1260x860')
        root.minsize(1000, 680)

        self._build_ui()
        self._try_find_addrs()
        self._connect()
        self._draw()

    # ------------------------------------------------------------------
    def _try_find_addrs(self):
        """Read symbol addresses from firmware_full.elf via nm."""
        import subprocess, os
        elf = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'firmware_full.elf')
        if not os.path.exists(elf):
            return
        try:
            out = subprocess.check_output(
                ['arm-none-eabi-nm', elf], text=True, stderr=subprocess.DEVNULL)
            sym = {}
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3:
                    sym[parts[2]] = int(parts[0], 16)
            if 'g_dbg'           in sym: self.dbg_addr         = sym['g_dbg']
            if 'g_ain'           in sym: self.ain_addr          = sym['g_ain']
            if 'g_uart_rx_count' in sym: self.uart_rx_addr      = sym['g_uart_rx_count']
            if 'g_uart_rx_errors'in sym: self.uart_rx_err_addr  = sym['g_uart_rx_errors']
        except Exception:
            pass

    # ------------------------------------------------------------------
    def _build_ui(self):
        # ---- Top status bar ------------------------------------------
        bar = tk.Frame(self.root, bg=PANEL, height=36)
        bar.pack(fill='x')
        bar.pack_propagate(False)
        self.lbl_conn   = tk.Label(bar, text='Connecting SWD…',
                                   bg=PANEL, fg=AMBER, font=FONT_L)
        self.lbl_conn.pack(side='left', padx=10, pady=4)
        self.lbl_fw     = tk.Label(bar, text='', bg=PANEL, fg=DIM, font=FONT)
        self.lbl_fw.pack(side='left', padx=6)
        self.lbl_uptime = tk.Label(bar, text='', bg=PANEL, fg=TEXT, font=FONT_B)
        self.lbl_uptime.pack(side='right', padx=10)

        # ---- Main layout: left waveforms | right panels --------------
        body = tk.Frame(self.root, bg=BG)
        body.pack(fill='both', expand=True, padx=4, pady=4)

        left  = tk.Frame(body, bg=BG)
        left.pack(side='left', fill='both', expand=True)
        right = tk.Frame(body, bg=BG, width=380)
        right.pack(side='right', fill='y', padx=(6, 0))
        right.pack_propagate(False)

        # ---- Left: ADC0 noise waveform (AIN0) ----------------------
        tk.Label(left, text='ADC0 — Noise / Antenna (AIN0)',
                 bg=BG, fg=AMBER, font=FONT_B).pack(anchor='w', padx=4)
        self.cv_adc0 = tk.Canvas(left, bg=BG, highlightthickness=0, height=180)
        self.cv_adc0.pack(fill='both', expand=True, padx=2)

        # ---- Left: AIN0-AIN11 live bar chart ------------------------
        tk.Label(left, text='AIN0–AIN11 — Live Channels (round-robin)',
                 bg=BG, fg=CYAN, font=FONT_B).pack(anchor='w', padx=4, pady=(6,0))
        self.cv_ain = tk.Canvas(left, bg=BG, highlightthickness=0, height=160)
        self.cv_ain.pack(fill='both', expand=True, padx=2)

        # ---- Left: UART loopback bar --------------------------------
        tk.Label(left, text='UART Loopback  (TX → header wire → RX)',
                 bg=BG, fg=GREEN, font=FONT_B).pack(anchor='w', padx=4, pady=(6,0))
        self.cv_uart = tk.Canvas(left, bg=BG, highlightthickness=0, height=100)
        self.cv_uart.pack(fill='both', expand=True, padx=2)

        # ---- Left: ADC1 temperature waveform -----------------------
        tk.Label(left, text='ADC1 — Temperature (internal sensor)',
                 bg=BG, fg=BLUE, font=FONT_B).pack(anchor='w', padx=4, pady=(6,0))
        self.cv_adc1 = tk.Canvas(left, bg=BG, highlightthickness=0, height=100)
        self.cv_adc1.pack(fill='both', expand=True, padx=2)

        # ---- Right panels -------------------------------------------
        # ADC0
        f0 = self._panel(right, 'ADC0 — Noise / Antenna')
        self.lbl_a0_val   = self._stat(f0, 'Now:',   AMBER)
        self.lbl_a0_chan  = self._stat(f0, 'Chan:',  CYAN)
        self.lbl_a0_scans = self._stat(f0, 'Scans:')
        self.lbl_a0_min   = self._stat(f0, 'Min:')
        self.lbl_a0_max   = self._stat(f0, 'Max:')
        self.lbl_a0_span  = self._stat(f0, 'Span:',  CYAN)
        self.lbl_a0_rate  = self._stat(f0, 'Rate:')
        self.lbl_a0_cnt   = self._stat(f0, 'Count:')

        # ADC1
        f1 = self._panel(right, 'ADC1 — Temperature')
        self.lbl_a1_c    = self._stat(f1, 'Temp:',  CYAN)
        self.lbl_a1_raw  = self._stat(f1, 'Raw:',   BLUE)
        self.lbl_a1_rate = self._stat(f1, 'Rate:')

        # DAC
        fd = self._panel(right, 'DAC Output')
        self.lbl_dac_val = self._stat(fd, 'Value:', PURPLE)
        self.dac_bar = tk.Canvas(fd, bg='#21262d', highlightthickness=0, height=12)
        self.dac_bar.pack(fill='x', padx=4, pady=2)

        # UART loopback stats
        fl = self._panel(right, 'UART Loopback')
        self.lbl_lb_tx  = self._stat(fl, 'TX:',     AMBER)
        self.lbl_lb_rx  = self._stat(fl, 'RX:',     GREEN)
        self.lbl_lb_pct = self._stat(fl, 'Return:', CYAN)
        self.lbl_lb_tot = self._stat(fl, 'Total RX:')

        # GPIO
        fg = self._panel(right, 'GPIO')
        self.lbl_p0_out = self._stat(fg, 'P0 out:')
        self.lbl_p1_in  = self._stat(fg, 'P1 in:',  GREEN)
        self.lbl_p2_in  = self._stat(fg, 'P2 in:',  GREEN)

        # System
        fs = self._panel(right, 'System')
        self.lbl_uptime2 = self._stat(fs, 'Uptime:')
        self.lbl_loops   = self._stat(fs, 'Loops:')
        self.lbl_wdt     = self._stat(fs, 'WDT:')
        self.lbl_errors  = self._stat(fs, 'Errors:', RED)
        self.lbl_flags   = self._stat(fs, 'Flags:',  DIM)

        # Buttons
        fb = tk.Frame(right, bg=BG)
        fb.pack(fill='x', pady=4)
        self._btn(fb, 'Reset Min/Max', 0x02)
        self._btn(fb, 'Save Cal',      0x01)
        self._btn(fb, 'SW Reset',      0xFF)

    def _panel(self, parent, title):
        f = tk.LabelFrame(parent, text=title, bg=PANEL, fg=TEXT,
                          font=FONT_B, bd=1, relief='groove', labelanchor='nw')
        f.pack(fill='x', padx=2, pady=2)
        return f

    def _stat(self, parent, label, color=None):
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill='x', padx=4, pady=1)
        tk.Label(row, text=label, bg=PANEL, fg=DIM, font=FONT_S,
                 width=8, anchor='w').pack(side='left')
        lbl = tk.Label(row, text='---', bg=PANEL, fg=color or TEXT,
                       font=FONT_S, anchor='e')
        lbl.pack(side='right')
        return lbl

    def _btn(self, parent, text, cmd_val):
        def _send():
            if not self.target:
                return
            try:
                self.target.write32(self.dbg_addr + 0x6C, cmd_val)
            except Exception:
                pass
        tk.Button(parent, text=text, command=_send,
                  bg=PANEL2, fg=TEXT, font=FONT_S, bd=1, relief='raised',
                  activebackground='#30363d').pack(
                      side='left', padx=3, expand=True, fill='x')

    # ------------------------------------------------------------------
    def _connect(self):
        if self.session:
            try:
                self.session.close()
            except Exception:
                pass
            self.session = None
            self.target  = None

        try:
            from pyocd.core.helpers import ConnectHelper
            self.session = ConnectHelper.session_with_chosen_probe(
                target_override='cortex_m', connect_mode='attach',
                options={'frequency': 100000})
            self.session.open()
            self.target = self.session.target

            magic = self.target.read32(self.dbg_addr)
            if magic != MAGIC_EXPECT:
                for off in range(0, 0x400, 4):
                    try:
                        if self.target.read32(0x20000000 + off) == MAGIC_EXPECT:
                            delta = (0x20000000 + off) - self.dbg_addr
                            self.dbg_addr         += delta
                            self.ain_addr         += delta
                            self.uart_rx_addr     += delta
                            self.uart_rx_err_addr += delta
                            magic = MAGIC_EXPECT
                            break
                    except Exception:
                        break

            if magic == MAGIC_EXPECT:
                try:
                    words = self.target.read_memory_block32(self.dbg_addr, STRUCT_WORDS)
                    self._tx_prev   = words[23]
                    self._rx_prev   = self.target.read32(self.uart_rx_addr)
                    self._rate_t    = time.time()
                    self._init_uart = True
                except Exception:
                    pass

                self.lbl_conn.config(
                    text=f'Connected  0x{self.dbg_addr:08X}  g_ain=0x{self.ain_addr:08X}',
                    fg=GREEN)
                self.running = True
                threading.Thread(target=self._reader, daemon=True).start()
            else:
                self.lbl_conn.config(
                    text=f'Bad magic 0x{magic:08X} — flash firmware?', fg=RED)
                self.root.after(4000, self._connect)

        except Exception as ex:
            self.lbl_conn.config(text=f'SWD error: {ex}', fg=RED)
            self.root.after(4000, self._connect)

    # ------------------------------------------------------------------
    def _reader(self):
        while self.running:
            try:
                words    = self.target.read_memory_block32(self.dbg_addr, STRUCT_WORDS)
                ain_raw  = self.target.read_memory_block32(self.ain_addr, 12)
                rx_count = self.target.read32(self.uart_rx_addr)
                rx_errs  = self.target.read32(self.uart_rx_err_addr)

                ain_signed = [s32(w) for w in ain_raw]

                d = {
                    'magic':        words[0],
                    'fw_ver':       words[1],
                    'uptime':       words[2],
                    'flags':        words[3],
                    'a0_raw':       words[4],
                    'a0_min':       words[5],
                    'a0_max':       words[6],
                    'a0_count':     words[7],
                    'a0_rate':      words[8],
                    'a1_raw':       words[10],
                    'a1_rate':      words[14],
                    'dac_val':      words[16],
                    'tick_ms':      words[18],
                    'wdt_pets':     words[20],
                    'gpio_p0_out':  words[21],
                    'loop_count':   words[22],
                    'uart_tx':      words[23],
                    'cal_valid':    words[24],
                    'error_total':  words[26],
                    # v2.0.0 fields
                    'adc0_chan':    words[28],
                    'adc0_scans':  words[29],
                    'gpio_p1_in':  words[30],
                    'gpio_p2_in':  words[31],
                    'ain':         ain_signed,
                    'uart_rx':     rx_count,
                    'uart_rx_errs':rx_errs,
                }

                now     = time.time()
                elapsed = now - self._rate_t
                if elapsed >= 1.0:
                    if self._init_uart:
                        self._tx_rate = int((d['uart_tx'] - self._tx_prev) / elapsed)
                        self._rx_rate = int((rx_count    - self._rx_prev)  / elapsed)
                        self.tx_hist.append(self._tx_rate)
                        self.rx_hist.append(self._rx_rate)
                    self._tx_prev   = d['uart_tx']
                    self._rx_prev   = rx_count
                    self._rate_t    = now
                    self._init_uart = True

                self.adc0_hist.append(s32(d['a0_raw']))
                self.adc1_hist.append(s32(d['a1_raw']))
                self.ain_snap = ain_signed

                with self._lock:
                    self._data = d

                self.root.after(0, self._update_ui)
                time.sleep(0.06)

            except Exception:
                self.running = False
                self.root.after(0, lambda: self.lbl_conn.config(
                    text='SWD disconnected — retrying…', fg=AMBER))
                self.root.after(4000, self._connect)
                return

    # ------------------------------------------------------------------
    def _update_ui(self):
        with self._lock:
            d = dict(self._data)
        if not d:
            return

        # Top bar
        v = d['fw_ver']
        self.lbl_fw.config(
            text=f"FW v{(v>>16)&0xFF}.{(v>>8)&0xFF}.{v&0xFF}  "
                 f"@ 0x{self.dbg_addr:08X}")
        up = d['uptime']
        self.lbl_uptime.config(
            text=f"Up {up//3600:02d}:{(up%3600)//60:02d}:{up%60:02d}")

        # ADC0
        a0     = s32(d['a0_raw'])
        a0_min = s32(d['a0_min'])
        a0_max = s32(d['a0_max'])
        span   = a0_max - a0_min
        nc = RED if span > 1_000_000 else (AMBER if span > 100_000 else GREEN)
        self.lbl_a0_val.config(text=str(a0))
        self.lbl_a0_chan.config(text=f"AIN{d['adc0_chan']}")
        self.lbl_a0_scans.config(text=f"{d['adc0_scans']:,}")
        self.lbl_a0_min.config(text=str(a0_min))
        self.lbl_a0_max.config(text=str(a0_max))
        self.lbl_a0_span.config(text=f'{span:,}', fg=nc)
        self.lbl_a0_rate.config(text=f"{d['a0_rate']} sps")
        self.lbl_a0_cnt.config(text=f"{d['a0_count']:,}")

        # ADC1 temperature
        tc = raw_to_c(d['a1_raw'])
        tf = tc * 9.0 / 5.0 + 32.0
        self.lbl_a1_c.config(text=f'{tc:.1f} °C  /  {tf:.1f} °F')
        self.lbl_a1_raw.config(text=str(s32(d['a1_raw'])))
        self.lbl_a1_rate.config(text=f"{d['a1_rate']} sps")

        # DAC
        self.lbl_dac_val.config(text=f"{d['dac_val']} / 4095")
        self.dac_bar.delete('all')
        bw   = self.dac_bar.winfo_width() or 200
        frac = min(d['dac_val'] / 4095.0, 1.0)
        self.dac_bar.create_rectangle(0, 0, int(bw * frac), 12,
                                       fill=PURPLE, outline='')

        # UART loopback
        tx_r = self._tx_rate
        rx_r = self._rx_rate
        pct  = int(rx_r * 100 / tx_r) if tx_r > 0 else 0
        self.lbl_lb_tx.config(text=f'{tx_r:,} B/s')
        self.lbl_lb_rx.config(
            text=f'{rx_r:,} B/s',
            fg=GREEN if rx_r > 500 else (AMBER if rx_r > 0 else RED))
        self.lbl_lb_pct.config(
            text=f'{pct}%',
            fg=GREEN if pct >= 75 else (AMBER if pct >= 30 else RED))
        self.lbl_lb_tot.config(text=f"{d['uart_rx']:,}")

        # GPIO
        self.lbl_p0_out.config(text=f"0x{d['gpio_p0_out']:02X}")
        self.lbl_p1_in.config( text=f"0x{d['gpio_p1_in']:02X}")
        self.lbl_p2_in.config( text=f"0x{d['gpio_p2_in']:02X}")

        # System
        self.lbl_uptime2.config(
            text=f"{up//3600:02d}:{(up%3600)//60:02d}:{up%60:02d}")
        self.lbl_loops.config(text=f"{d['loop_count']:,}")
        self.lbl_wdt.config(text=f"{d['wdt_pets']:,}")
        self.lbl_errors.config(
            text=str(d['error_total']),
            fg=RED if d['error_total'] > 0 else GREEN)

        flags = d['flags']
        fs = []
        if flags & STS_ADC0_RUNNING:  fs.append('ADC0')
        if flags & STS_ADC1_RUNNING:  fs.append('ADC1')
        if flags & STS_DAC_RUNNING:   fs.append('DAC')
        if flags & STS_UART_OK:       fs.append('UART')
        if flags & STS_WDT_ACTIVE:    fs.append('WDT')
        if flags & STS_TIMER_RUNNING: fs.append('TMR')
        if flags & STS_SPI1_READY:    fs.append('SPI1')
        if flags & STS_GPIO_READY:    fs.append('GPIO')
        if flags & STS_BOOT_COMPLETE: fs.append('BOOT')
        self.lbl_flags.config(text=' '.join(fs))

    # ------------------------------------------------------------------
    def _draw(self):
        self._draw_wave(self.cv_adc0, self.adc0_hist, AMBER)
        self._draw_wave(self.cv_adc1, self.adc1_hist, BLUE)
        self._draw_loopback(self.cv_uart)
        self._draw_ain(self.cv_ain)
        self.root.after(UPDATE_MS, self._draw)

    def _draw_wave(self, canvas, history, color):
        canvas.delete('all')
        W = canvas.winfo_width()  or 600
        H = canvas.winfo_height() or 160
        data = list(history)
        mn, mx = min(data), max(data)
        if mn == mx:
            mn -= 500
            mx += 500
        else:
            pad = (mx - mn) * 0.08
            mn -= pad; mx += pad

        def y(v):
            return H - int((v - mn) / (mx - mn) * H)

        for i in range(1, 4):
            canvas.create_line(0, H*i//4, W, H*i//4, fill='#21262d')
        if mn < 0 < mx:
            canvas.create_line(0, y(0), W, y(0), fill='#484f58', dash=(4, 4))

        step = W / max(len(data) - 1, 1)
        pts  = [(int(i * step), y(v)) for i, v in enumerate(data)]
        if len(pts) > 1:
            canvas.create_line(*[c for p in pts for c in p], fill=color, width=1)

        canvas.create_text(4,    4,      text=f'{mx:,.0f}', anchor='nw',
                           fill=DIM,  font=FONT_S)
        canvas.create_text(4,    H - 14, text=f'{mn:,.0f}', anchor='nw',
                           fill=DIM,  font=FONT_S)
        if data:
            canvas.create_text(W - 4, 4, text=str(data[-1]), anchor='ne',
                               fill=color, font=FONT_B)

    def _draw_ain(self, canvas):
        """Bar chart: AIN0-AIN11 current values, centred on zero."""
        canvas.delete('all')
        W = canvas.winfo_width()  or 600
        H = canvas.winfo_height() or 160

        data = list(self.ain_snap)
        if not data:
            return

        # Scale: symmetric, based on max absolute value
        peak = max(abs(v) for v in data)
        if peak < 1000:
            peak = 1000

        n      = 12
        margin = 4
        total  = W - 2 * margin
        bar_w  = total / n
        cy     = H // 2        # centre line
        max_h  = cy - 18       # leave room for label at top

        # Centre line
        canvas.create_line(0, cy, W, cy, fill='#484f58')

        for i, val in enumerate(data):
            x0   = margin + int(i * bar_w) + 1
            x1   = margin + int((i + 1) * bar_w) - 1
            norm = val / peak                            # -1.0 .. +1.0
            h    = int(abs(norm) * max_h)
            col  = AIN_COLORS[i % len(AIN_COLORS)]

            if val >= 0:
                canvas.create_rectangle(x0, cy - h, x1, cy, fill=col, outline='')
            else:
                canvas.create_rectangle(x0, cy, x1, cy + h, fill=col, outline='')

            # Channel label
            canvas.create_text((x0 + x1) // 2, H - 10,
                               text=str(i), fill=DIM, font=FONT_S)
            # Value label (above positive, below negative)
            vstr = f'{val/1e6:.1f}M' if abs(val) >= 500_000 else str(val)
            if val >= 0:
                ty = max(cy - h - 2, 2)
                canvas.create_text((x0 + x1) // 2, ty,
                                   text=vstr, fill=col, font=FONT_S, anchor='s')
            else:
                ty = min(cy + h + 2, H - 20)
                canvas.create_text((x0 + x1) // 2, ty,
                                   text=vstr, fill=col, font=FONT_S, anchor='n')

        # Peak label
        canvas.create_text(W - 4, 2,
                           text=f'±{peak/1e6:.1f}M' if peak >= 500_000 else f'±{peak}',
                           anchor='ne', fill=DIM, font=FONT_S)
        # AIN0 annotation
        canvas.create_text(margin + bar_w / 2, 2,
                           text='AIN0', anchor='n', fill=AMBER, font=FONT_S)

    def _draw_loopback(self, canvas):
        canvas.delete('all')
        W = canvas.winfo_width()  or 600
        H = canvas.winfo_height() or 100

        tx_data = list(self.tx_hist)
        rx_data = list(self.rx_hist)
        if not tx_data:
            return

        peak  = max(max(tx_data, default=1), 1)
        n     = len(tx_data)
        bar_w = max(W / n - 1, 2)
        gap   = 1

        for i, (tx, rx) in enumerate(zip(tx_data, rx_data)):
            x    = int(i * (bar_w + gap))
            h_tx = int(tx / peak * (H - 20))
            h_rx = int(rx / peak * (H - 20))
            canvas.create_rectangle(x, H - 16 - h_tx, x + bar_w,
                                    H - 16, fill=AMBER, outline='')
            canvas.create_rectangle(x, H - 16 - h_rx, x + bar_w,
                                    H - 16, fill=GREEN, outline='',
                                    stipple='gray50')

        canvas.create_rectangle(4,   H - 13, 16,  H - 3, fill=AMBER,  outline='')
        canvas.create_text(18, H - 8, text=f'TX {self._tx_rate:,} B/s',
                           anchor='w', fill=AMBER, font=FONT_S)
        canvas.create_rectangle(110, H - 13, 122, H - 3, fill=GREEN, outline='')
        canvas.create_text(124, H - 8, text=f'RX {self._rx_rate:,} B/s',
                           anchor='w', fill=GREEN, font=FONT_S)
        pct = int(self._rx_rate * 100 / self._tx_rate) if self._tx_rate > 0 else 0
        canvas.create_text(W - 4, H - 8, text=f'{pct}% return',
                           anchor='e', fill=CYAN, font=FONT_S)


if __name__ == '__main__':
    root = tk.Tk()
    FullViewer(root)
    root.mainloop()
