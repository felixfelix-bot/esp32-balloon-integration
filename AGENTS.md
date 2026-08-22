# AGENTS.md - AI Agent Instructions

## Project Overview
ESP32-C3 + NiceRF LoRa2021 (Semtech LR2021 Gen 4) pico balloon tracker AND mesh internet transport network. Solar/supercap power. Target weight: <14g (Mesh V1, night-off) or <9g (Minimal tracker).

Two-track project:
1. **Tracker** (`tracker/`): Single balloon telemetry, position reporting
2. **Mesh Stack** (`mesh-stack/`): Multi-balloon relay network for internet transport

## RF Driver: RadioLib

**IMPORTANT**: We use RadioLib for LR2021 communication — via a **forked submodule** (`tracker/firmware/components/RadioLib` → `github.com/felixfelix-bot/RadioLib`, branch `lr2021-flrc-511-match123`, patch mirrored in `patches/radiolib-lr2021-flrc511-match123.patch`). See `docs/radiolib-fork.md` for the why + update policy.
- Adds: `setFlrcSyncWordMatch()` (Match1/1+2/1+2+3), 511-byte FLRC payloads, CALIB_FRONT_END on Rx staging
- Supports: LoRa, FLRC, GFSK, OOK, LR-FHSS, O-QPSK, RTToF ranging
- Has native ESP-IDF support with HAL abstraction layer
- The custom LR2021 driver in `tracker/firmware/components/lr2021/` is DEPRECATED
- See `tracker/firmware/main/EspHalC3.h` for the ESP32-C3 hardware abstraction

## Inventory (Owned Parts)
- 20x ESP32-C3_Mini_V1 (Maker go, 22.52x18mm, USB-C, U.FL antenna) — was listed as "XIAO ESP32C3"
- 2x XIAO ESP32-C5
- 4x NiceRF LoRa2021 modules (19.72x15x2.2mm, 18-pin)
- 3x EBYTE E28-2G4M27S (SX1281, 2.4 GHz, +27 dBm PA built-in, SPI)
- 100x Solar cells 52x19mm (0.5V 400mA)
- 50x Solar cells 78x39mm (0.54W 0.5V)
- 30x DecoGlee 18" foil party balloons (short test flights only)
- ~43x Magenesis 10x2mm neodymium magnets (~1.21g each, test weights)
- 1x Pressure sensor + pump (for balloon testing)
- 1x MS300 jewelry scale (cannot weigh neodymium magnets — magnetic interference)
- 1x Digital calipers
- Double-sided copper clad FR4 boards (for toner transfer PCB fab)

## Build & Flash Commands

### Firmware (ESP-IDF v5.4.1)
```bash
source ~/esp/esp-idf/export.sh
cd tracker/firmware && idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # ESP32-C3_Mini_V1 (USB-C)
idf.py -p /dev/ttyUSB0 flash             # bare ESP-C3-12F
```

### Hardware (SKiDL + KiCad)
```bash
pip install skidl graphviz
cd tracker/hardware && python hub_board/hub_schematic.py
```

### Lint & Typecheck
```bash
cd tracker/firmware && idf.py reconfigure
ruff check tracker/hardware/
```

## Project Structure
```
tracker/firmware/main/         - Main application (C++, uses RadioLib)
tracker/firmware/components/   - Drivers (BMP280, power_manager, antenna_switch, sky66112)
                               NOTE: lr2021/ is deprecated, use RadioLib instead
tracker/hardware/hub_board/    - Central electronics board (SKiDL + KiCad)
tracker/hardware/hub_board_diy/- DIY v0.1 development hub board (toner transfer)
tracker/hardware/wing_board/   - 4x identical antenna+solar boards
tracker/hardware/footprints/   - Custom component footprint data (JSON)
tracker/ground-station/        - Antenna tracker + ground station software
tracker/ground-station/receiver/ - Ground station LoRa receiver (ESP32-C3, builds OK)
mesh-stack/                    - Mesh internet transport (separate AGENTS.md)
mesh-stack/ROADMAP.md          - Comprehensive mesh plan, link budgets, research checklist
mesh-stack/research/           - Research notes (erasure-coding, routing, tdma, etc.)
IMPLEMENTATION-PLAN.md         - Master implementation plan with checklists
docs/adr/                      - Architecture Decision Records (10 decisions)
docs/component-guide.md        - All parts with explanations and alternatives
docs/inventory.md              - Full inventory tracking
docs/plan-variants.md          - DIY / Minimal / Mittel / Komfort / Mesh V1 / Mesh V2
docs/antenna-strategy.md       - V1 omni + V2 directional + product research
docs/power-budget.md           - Tracker + mesh relay power analysis
  docs/balloon-pressure-test.md  - Mylar balloon test plan
  docs/balloon-test-results.md   - DecoGlee leak test data + community references
  docs/balloon-options-analysis.md - 7 balloon types compared with cost analysis
  docs/balloon-flight-lessons.md - Lessons from 80+ community flights (6 practitioners)
  docs/sx1280-repos-reference.md - Related SX1280 repos with git URLs
bom/BOM.md                     - Bill of Materials (prioritized)
```

## Plan Variants
| Variant | Weight | Antenna | FEM/SP4T | Target |
|---------|--------|---------|----------|--------|
| DIY v0.1 | ~15g | Wire dipole | No | Development |
| Minimal | ~8-9g | Wire dipole | No | Ultra-light flight |
| Mittel | ~12-13g | 1-2x PCB-Yagi | Optional | Recommended flight |
| Komfort | ~16.6g | 4x PCB-Yagi | Yes | Full features |
| **Mesh V1** | **~14g** | **Wire dipole** | **SKY66112** | **Mesh relay (night-off)** |
| **Mesh V2** | **~18-22g** | **PCB-Yagis** | **SKY66114 +30dBm** | **Mesh relay (night-active)** |

## Key Design Decisions (see docs/adr/)
1. ESP32-C3 as MCU (ADR-001)
2. LR2021 Gen 4 as LoRa chip (ADR-002)
3. Dual-track hardware: Dev board (ESP32-C3_Mini_V1) + Flight board (bare chip) (ADR-003)
4. 3D Yagi antenna structure: 4 wings + SP4T switch (ADR-004)
5. SKY66112-11 FEM for PA+LNA (ADR-005)
6. Supercapacitor power (Solar → Caps → LDO) (ADR-006)
7. Adaptive protocol (FLRC/LoRa/Sub-GHz) (ADR-007)
8. 24-byte binary telemetry with CRC-16 (ADR-008)
9. **V1 omnidirectional dipoles, V2 directional upgrade** (ADR-009)
10. **Adaptive TX power per TDMA slot** (ADR-010)
11. **Single balloon first, He 4.6, Minimal variant for first flight** (ADR-011)

## NiceRF LoRa2021 Pin Mapping (ESP32-C3_Mini_V1 Dev Board)
```
NiceRF Pin   Function    ESP32 GPIO  Silkscreen  Notes
Pin 1        VCC         3.3V        3V3
Pin 2,8,11,12,18  GND   GND         GND
Pin 3        MISO        GPIO2       D2          Strapping pin (OK as input)
Pin 4        MOSI        GPIO7       D7          JTAG label, usable as GPIO
Pin 5        SCK         GPIO6       D6          JTAG label, usable as GPIO
Pin 6        NSS         GPIO10      D10
Pin 7        BUSY        GPIO4       D4          JTAG label, usable as GPIO
Pin 9        ANT (Sub-GHz, 50 Ohm)
Pin 10       2.4G (2.4 GHz + S Band, 50 Ohm)
Pin 14       RST         GPIO3       D3
Pin 15       DIO9 (IRQ)  GPIO5       D5          JTAG label, usable as GPIO
Pin 16       DIO8        GPIO1       D1
Pin 17       DIO7        GPIO0       D0
```
RadioLib: Set `radio.irqDioNum = 9` and call `setDioFunction()` for DIO9 as IRQ.

GPIO4-7 are labeled "JTAG" on silkscreen but are general-purpose GPIO when JTAG not enabled.
GPIO8 (I2C SDA) is shared with onboard LED (inverted) — LED flickers during I2C (cosmetic only).
GPIO9 (I2C SCL) is shared with BOOT button — internal pullup is fine for I2C.
Using GPIO4-7 means no JTAG debug; use UART/printf via USB-C.
See `tracker/hardware/footprints/esp32c3-mini-v1.json` for full pinout data.

## Pin Assignment (ESP32-C3 bare, Flight Board)
```
GPIO7  = SPI_MOSI (LR2021)
GPIO2  = SPI_MISO (LR2021)
GPIO6  = SPI_SCLK (LR2021)
GPIO10 = SPI_CS   (LR2021 NSS)
GPIO3  = LR2021 RESET
GPIO4  = LR2021 BUSY
GPIO5  = LR2021 DIO9 (IRQ) -- note: NiceRF exposes DIO7/8/9, not DIO1/5
GPIO0  = ADC Supercap Voltage (or use LR2021 getVoltage())
GPIO20 = I2C_SDA (BMP280)  -- was GPIO8, moved to avoid strapping pin issue
GPIO21 = I2C_SCL (BMP280)  -- was GPIO9, moved to avoid strapping pin issue
GPIO1  = FEM TX_EN (SKY66112) -- optional
```

**IMPORTANT**: GPIO8 and GPIO9 are strapping pins on ESP32-C3:
- GPIO8 pulled HIGH at reset → can force download boot mode
- GPIO9 pulled LOW at reset (BOOT button) → download boot mode
- Do NOT use GPIO8/GPIO9 for I2C or any function with pull-up resistors
- The dev board (SuperMini) has an onboard LED on GPIO8 that causes this exact issue
- Flight board uses GPIO20/GPIO21 for I2C instead (safe, non-strapping)

## Antenna Strategy
- Sub-GHz (868 MHz): Wire dipole, omnidirectional, +22 dBm, ~480 km range
- 2.4 GHz: PCB Yagis on wing boards (Komfort) or wire dipole (Minimal)
- Ground station: Circular polarized (RHCP) antenna for 2.4 GHz to eliminate rotation loss
- See docs/antenna-strategy.md for details
- **V1: Omnidirectional wire dipoles** — ground station gain sufficient for 22 kbps @ 300 km
- **V2: Directional PCB Yagis** — 2-3x throughput upgrade when needed (ADR-009)

## Important Notes
- WiFi and Bluetooth MUST be disabled in sdkconfig.defaults (power saving)
- CPU clock should be set to 80 MHz (not 160 MHz) for power saving
- RadioLib is C++; app_main must be in a .cpp file with `extern "C" void app_main()`
- All wing boards are IDENTICAL (same PCB, same components)
- Solder joints connect wing boards to hub board (no connectors on flight board)
- Always run `idf.py build` after firmware changes to verify compilation
- FEM and SP4T are optional; start without them for DIY/Minimal
- **Flight board uses GPIO20/GPIO21 for I2C (NOT GPIO8/GPIO9)** — see strapping pin warning above
- **NiceRF LoRa2021 uses crystal oscillator (XTAL), NOT TCXO** — pass `tcxoVoltage=0` to `radio->begin()`. With TCXO config, oscillator fails to start (HF_XOSC_START_ERR) causing calibrate and setFrequency to fail.
- **RadioLib calibration error handling**: `RADIOLIB_DEBUG_BASIC` must be set in RadioLib component (not just main) for the debug branch. We patched `config()` to always continue on calibration failure.
- **SPI debug**: `RADIOLIB_DEBUG_SPI=1` and `RADIOLIB_DEBUG_BASIC=1` in main CMakeLists are PRIVATE — they only apply to the main component, not RadioLib
- **`radio->irqDioNum = 9` is MANDATORY** for LR2021 TX/RX — without it, DIO mapping is not configured and interrupts don't fire on GPIO5. Set before calling `begin()` or `beginFLRC()`.
- **LR2021 power limits**: Sub-GHz (150-1090 MHz) = -9 to +22 dBm; 2.4 GHz (2400-2500 MHz) = -19 to +12 dBm
- **ESP-IDF framework required for LR2021 TX** — PlatformIO Arduino framework cannot TX (returns TX_TIMEOUT -5)
- **`esp_task_wdt_deinit()`** needed in benchmarker — RX loop blocks cmd task and starves IDLE task watchdog
- **RX processing bottleneck ~15-20ms** — at 2600 kbps with 200B pkts, need 20ms spacing for 0% PER
- **LR2021 has native FIFO API** (unlike SX1280): `readRadioRxFifo()`, `getRxFifoLevel()` (uint16_t!), `configFifoIrq()`, `autoTxRx()`, `clearRxFifo()`. Access via `#define RADIOLIB_GODMODE 1` before `#include <RadioLib.h>` (zero-patch, no RadioLib file modifications needed)
- **WARNING: RADIOLIB_GODMODE BREAKS RX** — Using GODMODE silently corrupts radio configuration. All RX firmware MUST use public RadioLib API only. Do NOT use GODMODE in RX code.
- **Raw SPI bypass achieves 838.8 kbps** — Bypassing RadioLib with direct SPI commands (CMD_READ_RX_FIFO=0x0001, CMD_CLEAR_IRQ=0x0116, CMD_WRITE_TX_FIFO=0x0002, CMD_SET_TX=0x020D) achieves 8.3x improvement over RadioLib baseline (101.2 kbps). Per-packet processing: 188µs (was 14ms).
- **No-STBY continuous RX** — Keep radio in RX mode during FIFO read. Skip standby/setRx transition. Saves ~24ms per packet.
- **80 MHz is optimal for throughput** — 160 MHz gives WORSE results (775 vs 839 kbps) due to USB serial JTAG interrupt interference.
- **SPI FIFO read speed: 10.46 Mbps** — reading 255 bytes takes only 195µs. The 80 kbps bottleneck is NOT the SPI bus; it's per-packet processing overhead (standby + startReceive + PRBS + RTOS)
- **LR2021 ≠ SX1280** — different chip, different architecture. LR2021 has dedicated RX/TX FIFOs with threshold interrupts, auto-RX-TX mode, single-frame reads. SX1280 has flat 256B buffer with single-packet overwrite.

## Bench Test Results (2026-06-11)

See `mesh-stack/flrc-bench-espidf/RESULTS.md` for full data.

| Test | Band | Mode | Bit Rate | Pkts | PER | BER | TX Tput |
|------|------|------|----------|------|-----|-----|---------|
| L1 | 868 | LoRa SF9 | ~1 kbps | 10/10 | 0% | 0% | 0.2 kbps |
| F1-F4 | 868 | FLRC | 260-2600 kbps | 100/100 each | 0% | 0% | 4-40 kbps |
| Burst | 868 | FLRC 2600 | 200B, no delay | 200→100 | 50% | 0% | 167 kbps |
| Sustained | 868 | FLRC 2600 | 200B, 20ms | 200/200 | 0% | 0% | 80 kbps |
| 2G4 | 2450 | FLRC | 1300-2600 | 100/100 | 0% | 0% | 40 kbps |
| Power | 868 | FLRC 1300 | -6 to +22 dBm | All pass | 0% | 0% | 40 kbps |
| Power | 2450 | FLRC 1300 | -16 to +12 dBm | 6/8 pass | ≤2% | 0% | 40 kbps |
| PktSz | 868 | FLRC 1300 | 20-255 bytes | All pass | 0% | 0% | 8-68 kbps |

**Key result**: Max sustained throughput = **80 kbps** (FLRC 2600 kbps, 200B, 20ms spacing).
**Deployment config**: 2.4 GHz FLRC 1300-2600 kbps at +12 dBm with wire dipoles.

## Balloon Strategy

**Short test flights (DecoGlee, owned):** 30x DecoGlee 18" foil, 4.8g net lift, 0.15 g/day leak rate. Use for 3-8 day shakedown flights. Heat seal + Kapton tape. 6-7 balloons with cut-down for Mesh V1.

**Long-duration flights (Yokohama + He 4.6, to purchase):** Yokohama 32" Crystal Clear Sphere Balloon valveless 10-pack €105.95 from https://www.yokohamaballoon.com/. Nylon/PE laminate (NOT foil). Industrial helium (grade 4.6, 99.996%) from Air Liquide ALbee Fly system. 67% circumnavigation rate with ultra-pure He (Ruthroff data). Our 9-14g payload is well within proven range (Ruthroff flew 14-22g payloads). Hydrogen deferred to future — He 4.6 is safer, higher success rate, and can be handled indoors. See ADR-011.

**Key references:**
- Ruthroff (37 flights, Yokohama + H2/He): https://www.theastroimager.com/picoballoning/pico-ballooning/
- KI4MCW (31 flights, party balloons): https://sites.google.com/site/ki4mcw/Home/pico-balloonery
- K9YO (beginner guide): https://sites.google.com/view/picoballoonsbyk9yo/beginners-guide
- Klofas/SF-HAB (5+ launches): https://www.klofas.com/blog/tag/picoballoon.html
- IEEE Spectrum (David Schneider): https://spectrum.ieee.org/explore-stratosphere-diy-pico-balloon
- Pico Balloon mailing list (1,152 members): https://groups.io/g/picoballoon
- Traquito/Picoballoons.net: https://www.picoballoons.net/
- WSPRnet (global spot database): https://wsprnet.org/
- Yokohama balloons: https://www.yokohamaballoon.com/
- SBS balloons: https://www.scientificballoonsolutions.com/products/
- HYSPLIT (NOAA trajectory prediction): https://ready.arl.noaa.gov/HYSPLIT.php
