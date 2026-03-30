# EVAL-ADICUP360 DIP Switch Routing Map

## Switch Schematic (from board docs)

The board has 4 DPDT analog switches (CAS-220B1) arranged in two cascaded pairs:

```
                    ARDUINO PWMH / SPI
                    ┌─── P0_1_IO ───┐
                    │    P0_2_IO    │
     ┌──────────┐   │               │   ┌──────────┐
P0.1─┤ S1       ├───┤pos=1          │   │ S3       ├─── UART1_RX_DEBUG ─→ DEBUG USB
P0.2─┤ (pos 0)──├───┼───────────────┼──→│ (pos 0)──├─── UART1_TX_DEBUG ─→ DEBUG USB
     └──────────┘   │               │   └──────────┘
                    └───────────────┘

                    ARDUINO PWML
                    ┌─── P0_7_IO ───┐
                    │    P0_6_IO    │
     ┌──────────┐   │               │   ┌──────────┐
P0.7─┤ S2       ├───┤pos=1          │   │ S4       ├─── RXD ─→ USER USB (FTDI COM5)
P0.6─┤ (pos 0)──├───┼───────────────┼──→│ (pos 0)──├─── TXD ─→ USER USB (FTDI COM5)
     └──────────┘   │               │   └──────────┘
                    └───────────────┘
```

## Switch Position Table

Position 0 = DOWN, Position 1 = UP

### P0.6/P0.7 routing (S2 + S4)

| S2 | S4 | P0.6 (RX) goes to | P0.7 (TX) goes to |
|----|-----|-------------------|-------------------|
| 0  | 0   | **FTDI RXD (COM5)** | **FTDI TXD (COM5)** |
| 0  | 1   | (unused?) | (unused?) |
| 1  | 0   | Arduino PWML header | Arduino PWML header |
| 1  | 1   | Arduino PWML header | Arduino PWML header |

**For UART via COM5 (FTDI): S2=0, S4=0 (both DOWN)**

### P0.1/P0.2 routing (S1 + S3)

| S1 | S3 | P0.1 (RX) goes to | P0.2 (TX) goes to |
|----|-----|-------------------|-------------------|
| 0  | 0   | **DAPLink (COM6)** | **DAPLink (COM6)** |
| 0  | 1   | (unused?) | (unused?) |
| 1  | 0   | Arduino PWMH/SPI header | Arduino PWMH/SPI header |
| 1  | 1   | Arduino PWMH/SPI header | Arduino PWMH/SPI header |

**For UART via COM6 (DAPLink): S1=0, S3=0 (both DOWN)**

## Common Configurations

| Config | S1 | S2 | S3 | S4 | Result |
|--------|----|----|----|----|--------|
| **UART on COM5+COM6** | 0 | 0 | 0 | 0 | P0.6/P0.7→FTDI, P0.1/P0.2→DAPLink |
| UART on COM5 only | X | 0 | X | 0 | P0.6/P0.7→FTDI |
| UART on COM6 only | 0 | X | 0 | X | P0.1/P0.2→DAPLink |
| All to headers | 1 | 1 | 1 | 1 | P0.1/P0.2→PWMH, P0.6/P0.7→PWML |
| Factory default? | 1 | 1 | 1 | 1 | All to Arduino headers |

## Header Pin Mapping (right side of board, top to bottom)

From the board silkscreen:
```
SCLK1   ← P0.0 SPI1 clock
MISO1   ← P0.0 SPI1 MISO (shared with SCLK1?)
MOSI1   ← SPI1 MOSI
CS      ← SPI1 chip select
P0_4    ← LED2 (green)
P0_3    ← GPIO / IRQ0
P0_2    ← UART TX (func3) / MOSI1 / SDA
TX      ← Switched UART TX output (goes to header, NOT to FTDI)
RX      ← Switched UART RX output (goes to header, NOT to FTDI)
P0_7    ← UART TX (func2) / POR
P0_6    ← UART RX (func1) / IRQ2
SDA     ← I2C data
SCL     ← I2C clock
```

**WARNING:** The TX/RX header pins are the Arduino-routed signals, NOT the FTDI.
They only carry UART when switches route to Arduino headers (S2=1 or S4=1).

## Key Lesson Learned

The wiki documentation images show switch positions that DON'T match the actual
schematic routing. The switch_schematic.png from the wiki clearly shows:
- S2(pos 0) + S4(pos 0) = FTDI (USER USB)
- S1(pos 0) + S3(pos 0) = DAPLink (DEBUG USB)

Position 0 = DOWN on the physical DIP switch.
ALL switches DOWN = both UART pairs routed to USB serial ports.
