"""Regression gate: the LR2021 SPI clock must stay inside the datasheet maximum.

The LR2021 datasheet caps the SPI clock at 16 MHz. This repo carries three copies
of the ESP32-C3 radio HAL and the flrc-bench copy
(``mesh-stack/flrc-bench-espidf/main/EspHalC3.h``) sat at **18 MHz** — above spec,
the same defect class as balloon-fresh P0.4 (``t_0b2f5534``, GDMA HAL at 40 MHz).
The other two copies (``tracker/firmware`` and ``tracker/ground-station/receiver``)
run at 2 MHz and are compliant.

These tests are host-only: they parse the firmware sources, so they run in CI and
without any board attached. They exist so the clock cannot silently drift back
above spec in any of the HAL copies.

RED demonstration (the suite detects the original violation) can be reproduced
against the pre-fix revision, or against any file via ``C3_HAL_PATH``::

    cd ~/repos/esp32-balloon-integration-fresh
    git show HEAD:mesh-stack/flrc-bench-espidf/main/EspHalC3.h > /tmp/EspHalC3_18MHz.h
    C3_HAL_PATH=/tmp/EspHalC3_18MHz.h /opt/miniconda/bin/python3.13 -m pytest \
        tests/test_c3_spi_clock.py -v
"""

import os
import re

import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH_MAIN = os.path.join(REPO_ROOT, "mesh-stack", "flrc-bench-espidf", "main")
HAL_PATH = os.environ.get("C3_HAL_PATH", os.path.join(BENCH_MAIN, "EspHalC3.h"))

# LR2021 datasheet maximum SPI clock.
DATASHEET_MAX_HZ = 16_000_000

# Every ESP32-C3 radio HAL copy in this tree (repo-root relative). The 16 MHz
# datasheet limit applies to all of them, so a new/forked copy cannot drift.
ALL_HAL_COPIES = (
    "mesh-stack/flrc-bench-espidf/main/EspHalC3.h",
    "tracker/firmware/main/EspHalC3.h",
    "tracker/ground-station/receiver/main/EspHalC3.h",
)

# C3 bench applications that talk to the LR2021 over SPI (all under BENCH_MAIN).
# ``spi_loopback.cpp`` is deliberately excluded: it sweeps a bare jumper wire with
# no LR2021 attached, so the radio datasheet cap does not apply to it.
BENCH_RADIO_APPS = (
    "bench_main.cpp",
    "autonomous_main.cpp",
    "range_test.cpp",
    "fast_rx.cpp",
    "profile_rx.cpp",
    "fifo_test.cpp",
    "fifo_tx.cpp",
    "continuity_test.cpp",
    "fips_bridge.cpp",
)

# Raw bench applications whose boot banner must self-report the clock it configures.
RAW_BENCH_APPS = ("bench_main.cpp",)


def _read(path: str) -> str:
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def _eval_int_expr(expr: str) -> int:
    """Evaluate a simple integer arithmetic expression such as ``16 * 1000 * 1000``."""
    if not re.fullmatch(r"[\d\s*+()/]+", expr):
        raise AssertionError(f"unexpected non-arithmetic SPI clock expression: {expr!r}")
    return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - regex-validated


def _parse_spi_hz_macro(text: str) -> int:
    match = re.search(r"#define\s+ESPHAL_C3_SPI_HZ\s+\(([^)]*)\)", text)
    if not match:
        match = re.search(r"#define\s+ESPHAL_C3_SPI_HZ\s+([0-9\s*+()]+)", text)
    assert match, "ESPHAL_C3_SPI_HZ is not defined in the C3 radio HAL"
    return _eval_int_expr(match.group(1).strip())


def _clock_assignments(text: str):
    """Return the literal right-hand sides of every dev_cfg.clock_speed_hz assignment."""
    return re.findall(r"dev_cfg\.clock_speed_hz\s*=\s*([^;]+);", text)


def test_hal_file_exists():
    assert os.path.isfile(HAL_PATH), f"C3 radio HAL not found: {HAL_PATH}"


class TestC3SpiClockDatasheetCompliance:
    """ESPHAL_C3_SPI_HZ and every LR2021 radio SPI device config stay <= 16 MHz."""

    def test_spi_hz_macro_within_datasheet_max(self):
        hz = _parse_spi_hz_macro(_read(HAL_PATH))
        assert hz <= DATASHEET_MAX_HZ, (
            f"ESPHAL_C3_SPI_HZ = {hz} Hz exceeds the LR2021 datasheet maximum "
            f"of {DATASHEET_MAX_HZ} Hz"
        )

    def test_spi_hz_macro_is_arithmetic_and_nonzero(self):
        hz = _parse_spi_hz_macro(_read(HAL_PATH))
        assert hz > 0, "ESPHAL_C3_SPI_HZ must be a positive clock value"

    def test_radio_device_config_uses_the_macro(self):
        """dev_cfg.clock_speed_hz must be macro-driven in the bench HAL, and no HAL
        copy may hard-code a value above the datasheet maximum."""
        text = _read(HAL_PATH)
        assignments = _clock_assignments(text)
        assert assignments, "no dev_cfg.clock_speed_hz assignment found in the HAL"
        assert any(
            "ESPHAL_C3_SPI_HZ" in a for a in assignments
        ), f"radio SPI device clock is not driven by ESPHAL_C3_SPI_HZ: {assignments}"

        for rel in ALL_HAL_COPIES:
            path = os.path.join(REPO_ROOT, rel)
            if not os.path.isfile(path):
                continue
            for literal in _clock_assignments(_read(path)):
                if "ESPHAL_C3_SPI_HZ" in literal:
                    continue
                for n in re.findall(r"\d+", literal):
                    assert int(n) <= DATASHEET_MAX_HZ, (
                        f"{rel}: literal radio SPI clock {literal!r} exceeds the "
                        f"LR2021 datasheet maximum of {DATASHEET_MAX_HZ} Hz"
                    )

    def test_no_literal_radio_spi_clock_above_spec(self):
        """No radio source may hard-code an SPI clock above the 16 MHz datasheet max."""
        offenders = []
        paths = [os.path.join(REPO_ROOT, rel) for rel in ALL_HAL_COPIES]
        paths += [os.path.join(BENCH_MAIN, name) for name in BENCH_RADIO_APPS]
        for path in paths:
            if not os.path.isfile(path):
                continue
            rel = os.path.relpath(path, REPO_ROOT)
            for lineno, line in enumerate(_read(path).splitlines(), 1):
                if "clock_speed_hz" not in line or "ESPHAL_C3_SPI_HZ" in line:
                    continue
                for n in re.findall(r"\b(\d{7,9})\b", line):
                    if int(n) > DATASHEET_MAX_HZ:
                        offenders.append(f"{rel}:{lineno}: {line.strip()}")
        assert not offenders, "radio SPI clock above datasheet max:\n" + "\n".join(offenders)

    def test_firmware_reports_the_same_clock_it_configures(self):
        """Runtime banners must print ESPHAL_C3_SPI_HZ so a flashed board self-reports."""
        reporters = [
            name for name in RAW_BENCH_APPS
            if os.path.isfile(os.path.join(BENCH_MAIN, name))
        ]
        assert reporters, "expected at least one raw C3 bench application in main/"
        for name in reporters:
            text = _read(os.path.join(BENCH_MAIN, name))
            assert re.search(r'SPI clock = %d Hz\\n",\s*ESPHAL_C3_SPI_HZ', text), (
                f"{name} does not report ESPHAL_C3_SPI_HZ in its startup banner"
            )


class TestC3SpiClockRegressionAtOriginalValue:
    """Guard against the historical violations (40 MHz GDMA HAL, 18 MHz legacy)."""

    @pytest.mark.parametrize("bad_hz", [40_000_000, 18_000_000])
    def test_historical_violations_would_be_rejected(self, bad_hz):
        fake = f"#define ESPHAL_C3_SPI_HZ   ({bad_hz // 1_000_000} * 1000 * 1000)\n"
        assert _parse_spi_hz_macro(fake) > DATASHEET_MAX_HZ
