#!/usr/bin/env python3
"""Host gate for the ESP32-C3 BENCH console firmware (HARM-T4).

Compiles ``tracker/firmware/components/bench`` (vendored E80 console trio +
the board-independent console state machine) with the host gcc/g++ and runs the
on-host parity suite in ``components/bench/test/test_bench_console.c``:

* PRBS-15 fill/verify vs the spec §4 golden bytes
* CRC-16/CCITT-FALSE vs the spec §5 golden vectors
* ``bench_pkt_format()`` byte-exact against the E80 25-column ``PKT`` goldens
  (and 24-column append-only tolerance)
* ``bench_cmd_parse()`` golden parses (spec §2.2-§2.7)
* the console state machine's reply strings, LEN caps (§6), frequency windows
  (§9), PA caps, STAT accounting (§2.10) and PKT emission (§3)

No hardware is touched: the firmware is compiled for the host with a fake
``bench_radio_ops_t`` (see the C test). The ESP32 build itself is verified by
``idf.py build`` (default config) and ``idf.py -DSDKCONFIG=sdkconfig.bench``
(CONFIG_BENCH_CONSOLE=y, i.e. the bench sources actually compile).

Run: ``/usr/bin/python3 -m pytest tests/test_bench_host.py -v``
"""

import os
import subprocess
import tempfile

import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(REPO_ROOT, "tracker", "firmware", "components", "bench")
GOLDEN = os.path.join(REPO_ROOT, "tests", "golden")


def _compile_and_run():
    sources = [
        os.path.join(BENCH, name)
        for name in (
            "bench_cmd.c",
            "bench_pkt.c",
            "prbs.c",
            "bench_payload.c",
            "bench_stats.c",
            "buffer.c",
            "bench_console.c",
        )
    ]
    for src in sources:
        assert os.path.isfile(src), f"missing bench source: {src}"

    test_src = os.path.join(BENCH, "test", "test_bench_console.c")
    assert os.path.isfile(test_src), f"missing host test: {test_src}"

    binary = os.path.join(tempfile.gettempdir(), "test_bench_console")
    cmd = [
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O0",
        "-I", os.path.join(BENCH, "include"),
        f'-DGOLDEN_DIR="{GOLDEN}"',
        test_src, *sources,
        "-o", binary,
    ]
    comp = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if comp.returncode != 0:
        pytest.fail("compile failed:\n" + " ".join(cmd) + "\n" + comp.stderr)

    run = subprocess.run([binary], capture_output=True, text=True, timeout=120)
    print(run.stdout)
    if run.returncode != 0:
        pytest.fail(f"host bench suite failed (rc={run.returncode}):\n"
                    f"{run.stdout}\n{run.stderr}")
    return run.stdout


class TestBenchConsoleHost:
    def test_bench_console_parity(self):
        out = _compile_and_run()
        assert "FAIL" not in out, out
        # the C test prints its own pass/fail tally; require a zero-failure tally
        marker = "=== Results: "
        assert marker in out, out
        tally = out.split(marker, 1)[1].split()[0]
        passed, total = (int(x) for x in tally.split("/"))
        assert passed == total, out
        assert total >= 40, f"expected a substantive suite, got {tally}"
