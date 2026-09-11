# Changelog

All notable changes to `flrc-bench-espidf` will be documented in this file.

## [Unreleased]

### Fixed
- **ESP32-C3 LR2021 SPI clock brought inside the datasheet maximum (18 MHz → 16 MHz).**
  `main/EspHalC3.h` hard-coded `dev_cfg.clock_speed_hz = 18000000;` — above the
  LR2021 datasheet SPI maximum of 16 MHz (same defect class as balloon-fresh P0.4
  `t_0b2f5534`, whose GDMA HAL sat at 40 MHz). The clock is now a single source of
  truth, `ESPHAL_C3_SPI_HZ = (16 * 1000 * 1000)`, and `dev_cfg.clock_speed_hz`
  reads that macro. This is the third copy of the C3 radio HAL in this tree; the
  other two (`tracker/firmware/main/EspHalC3.h` and its symlinked
  `tracker/ground-station/receiver/main/EspHalC3.h`) run at 2 MHz and are already
  compliant — they are left untouched, but the new regression gate now covers them.
  - Regression gate: `tests/test_c3_spi_clock.py` (host-only, 8 tests) — RED on the
    pre-fix revision (5 failed), GREEN after the edit (8 passed). It parses every
    HAL copy, asserts the macro evaluates ≤ 16 MHz, that the device config is
    macro-driven, that no radio source hard-codes a clock above spec, and that the
    interactive bench app reports the same macro it configures.
  - Boot banner: `main/bench_main.cpp` now prints `SPI clock = <ESPHAL_C3_SPI_HZ> Hz`
    at startup so a flashed board self-reports the clock it actually configured.
  - Build: clean ESP-IDF v5.4.1 `esp32c3` build (1051/1051 steps).
  - Artifact: the compiled `spi_bus_add_device()` call-site constant is 16 000 000
    and no 18 MHz / 40 MHz constant survives in the ELF.
  - `main/spi_loopback.cpp` sweeps 1/4/8/18 MHz across a bare MOSI→MISO jumper with
    no radio attached; it is deliberately out of scope for the 16 MHz gate and now
    says so in a comment.
