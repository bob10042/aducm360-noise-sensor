# EVAL-ADICUP360 Demo Project Catalog

27 projects in `github.com/analogdevicesinc/EVAL-ADICUP360` under `projects/`

## Projects Using Internal ADC (most relevant to us)

| Project | What | Internal ADC | DAC | UART Pins |
|---------|------|-------------|-----|-----------|
| cn0338 (NDIR gas) | Dual ADC0+ADC1, CLI, flash storage | ADC0+ADC1 | No | P0.1/P0.2 |
| cn0394 (RTD+TC) | Dual ADC, IEXC current sources, cal | ADC0+ADC1 | No | P0.6/P0.7 |
| cn0359 (conductivity) | ADC+DAC+PWM, LCD, encoder, buzzer, flash | ADC+DAC | Yes | P0.6/P0.7 |
| cn0359_reva | Same + DMA driver | ADC+DAC+DMA | Yes | P0.6/P0.7 |

## Projects Using External ADC over SPI

| Project | External ADC | What |
|---------|-------------|------|
| cn0216 | AD7791 (24-bit) | Weigh scale |
| cn0326 | AD7793 (24-bit) | pH measurement |
| cn0336 | AD7091R (12-bit) | 4-20mA current |
| cn0337 | AD7091R (12-bit) | RTD temperature |
| cn0357 | AD7790 | Gas sensor |
| cn0391 | AD7124 (24-bit) | Temperature |
| cn0395 | AD7988 (SAR) | Gas + heater |
| cn0396 | AD7798 | Dual gas |
| cn0397 | AD7798 | Light intensity |
| cn0398 | AD7124 (24-bit) | pH + moisture |
| cn0411 | AD7124 + AD5683 DAC | Conductivity |

## UART Pin Usage

| Pins | Count | Projects |
|------|-------|----------|
| P0.6/P0.7 | 21 | Nearly all demos |
| P0.1/P0.2 | 2 | cn0337, cn0338 |
| Both | 2 | cli, test_project |
| None | 2 | blink, adxl362 |

## Key Reference Projects

1. **cn0359_reva** — Best reference for internal ADC+DAC+PWM with full HAL
2. **cn0394** — Best reference for dual internal ADC with IEXC and calibration
3. **cn0338** — Best reference for dual ADC with CLI and flash storage
4. **cli** — Best reference for UART command-line interface
5. **blink** — Simplest GPIO example (our starting point)
