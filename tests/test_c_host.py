import glob
import os
import subprocess
import tempfile

import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPONENTS = os.path.join(REPO_ROOT, "tracker", "firmware", "components")


HOST_STUBS = os.path.join(REPO_ROOT, "tests", "host_stubs")


def _compile_and_run_c(test_src, sources, include_dirs, link_flags=None, cxx=False, extra_cflags=None):
    if link_flags is None:
        link_flags = []
    if extra_cflags is None:
        extra_cflags = []
    compiler = "g++" if cxx else "gcc"
    std_flag = "-std=c++17" if cxx else "-std=c11"
    binary = os.path.join(tempfile.gettempdir(), os.path.splitext(os.path.basename(test_src))[0])

    cmd = [compiler, std_flag, "-Wall", "-Wextra", "-g", "-O0"]
    cmd.extend(["-I", HOST_STUBS])
    for d in include_dirs:
        cmd.extend(["-I", d])
    cmd.extend(extra_cflags)
    cmd.append(test_src)
    cmd.extend(sources)
    cmd.extend(["-o", binary])
    cmd.extend(link_flags)

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        pytest.fail(f"Compile failed:\n{' '.join(cmd)}\n{result.stderr}")
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        pytest.fail(f"Test failed (rc={result.returncode}):\n{result.stdout}\n{result.stderr}")
    return result.stdout


def _compile_with_makefile(makefile_dir):
    result = subprocess.run(
        ["make", "-C", makefile_dir, "test"],
        capture_output=True, text=True, timeout=60,
    )
    if result.returncode != 0:
        pytest.fail(f"make test failed in {makefile_dir}:\n{result.stdout}\n{result.stderr}")
    return result.stdout


class TestErasure:
    def test_erasure_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "erasure", "test", "test_erasure.c"),
            [os.path.join(COMPONENTS, "erasure", "erasure.c")],
            [os.path.join(COMPONENTS, "erasure", "include")],
        )
        assert "5/5 passed" in out


class TestTDMA:
    def test_tdma_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "tdma", "test", "test_tdma.c"),
            [os.path.join(COMPONENTS, "tdma", "tdma.c")],
            [os.path.join(COMPONENTS, "tdma", "include")],
        )
        assert "12/12 passed" in out


class TestLR2021FLRC:
    def test_lr2021_flrc_match123_host(self):
        # RadioLib sources: env override (fork checkout) or the pinned submodule
        rl_src = os.environ.get("RADIOLIB_SRC") or os.path.join(COMPONENTS, "RadioLib")
        rl_sources = (
            glob.glob(os.path.join(rl_src, "src", "Module.cpp"))
            + glob.glob(os.path.join(rl_src, "src", "modules", "LR11x0", "*.cpp"))
            + glob.glob(os.path.join(rl_src, "src", "modules", "LR2021", "*.cpp"))
            + glob.glob(os.path.join(rl_src, "src", "utils", "Cryptography.cpp"))
        )
        assert len(rl_sources) > 5, f"no RadioLib sources found under {rl_src}"
        out = _compile_and_run_c(
            os.path.join(REPO_ROOT, "tests", "src", "lr2021_flrc_match123", "test_lr2021_flrc_match123.cpp"),
            rl_sources,
            [rl_src, os.path.join(rl_src, "src"), os.path.join(rl_src, "src", "modules", "LR2021")],
            cxx=True,
        )
        assert "ALL CHECKS PASSED" in out


class TestNostrStore:
    def test_nostr_store_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "nostr_store", "test", "test_nostr_store.c"),
            [os.path.join(COMPONENTS, "nostr_store", "nostr_store.c")],
            [os.path.join(COMPONENTS, "nostr_store", "include")],
        )
        assert "7/7 passed" in out


class TestMicroECC:
    def test_micro_ecc_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "micro_ecc", "test", "test_micro_ecc.c"),
            [
                os.path.join(COMPONENTS, "micro_ecc", "uECC.c"),
            ],
            [os.path.join(COMPONENTS, "micro_ecc", "include")],
            extra_cflags=["-DESP32"],
        )
        assert "5/5" in out


class TestFIPSTransport:
    def test_fips_transport_host(self):
        makefile_dir = os.path.join(COMPONENTS, "fips_transport", "test")
        if os.path.exists(os.path.join(makefile_dir, "Makefile")):
            out = _compile_with_makefile(makefile_dir)
            assert "13/13" in out
        else:
            pytest.skip("No Makefile for fips_transport")


class TestWirehair:
    def test_wirehair_host(self):
        wirehair_dir = os.path.join(COMPONENTS, "wirehair")
        out = _compile_and_run_c(
            os.path.join(wirehair_dir, "test", "test_wirehair.cpp"),
            [
                os.path.join(wirehair_dir, "gf256.cpp"),
                os.path.join(wirehair_dir, "wirehair.cpp"),
                os.path.join(wirehair_dir, "WirehairCodec.cpp"),
                os.path.join(wirehair_dir, "WirehairTools.cpp"),
            ],
            [
                os.path.join(wirehair_dir, "include"),
                wirehair_dir,
            ],
            cxx=True,
            extra_cflags=["-DCAT_ALL_ORIGINAL", "-mssse3", "-Wno-implicit-fallthrough", "-Wno-restrict"],
        )
        assert "9/9 passed" in out


class TestPipeline:
    def test_pipeline_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "pipeline", "test", "test_pipeline.c"),
            [
                os.path.join(COMPONENTS, "pipeline", "pipeline.c"),
                os.path.join(COMPONENTS, "erasure", "erasure.c"),
                os.path.join(COMPONENTS, "frag", "frag.c"),
            ],
            [
                os.path.join(COMPONENTS, "pipeline", "include"),
                os.path.join(COMPONENTS, "erasure", "include"),
                os.path.join(COMPONENTS, "frag", "include"),
            ],
        )
        assert "9/9 passed" in out


class TestTelemetry:
    def test_telemetry_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "telemetry", "test", "test_telemetry.c"),
            [os.path.join(COMPONENTS, "telemetry", "telemetry.c")],
            [os.path.join(COMPONENTS, "telemetry")],
        )
        assert "11/11 passed" in out


class TestGPS:
    def test_gps_nmea_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "gps", "test", "test_gps.c"),
            [],
            [os.path.join(COMPONENTS, "gps")],
            link_flags=["-lm"],
        )
        assert "8/8 passed" in out


class TestFrag:
    def test_frag_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "frag", "test", "test_frag.c"),
            [os.path.join(COMPONENTS, "frag", "frag.c")],
            [os.path.join(COMPONENTS, "frag", "include")],
        )
        assert "13/13 passed" in out


class TestBMP280:
    def test_bmp280_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "bmp280", "test", "test_bmp280.c"),
            [],
            [os.path.join(COMPONENTS, "bmp280")],
            link_flags=["-lm"],
        )
        assert "6/6 passed" in out


class TestAntennaSwitch:
    def test_antenna_switch_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "antenna_switch", "test", "test_antenna_switch.c"),
            [],
            [os.path.join(COMPONENTS, "antenna_switch")],
        )
        assert "7/7 passed" in out


class TestSKY66112:
    def test_sky66112_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "sky66112", "test", "test_sky66112.c"),
            [],
            [os.path.join(COMPONENTS, "sky66112")],
        )
        assert "9/9 passed" in out


class TestCLI:
    def test_cli_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "cli", "test", "test_cli.c"),
            [],
            [os.path.join(COMPONENTS, "cli", "include")],
        )
        assert "8/8 passed" in out


class TestPowerManager:
    def test_power_manager_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "power_manager", "test", "test_power_manager.c"),
            [],
            [
                os.path.join(COMPONENTS, "power_manager"),
                os.path.join(HOST_STUBS),
            ],
        )
        assert "5/5 passed" in out


class TestMeshAdapter:
    def test_mesh_adapter_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "mesh_adapter", "test", "test_mesh_adapter.c"),
            [
                os.path.join(COMPONENTS, "mesh_adapter", "mesh_adapter.c"),
                os.path.join(COMPONENTS, "pipeline", "pipeline.c"),
                os.path.join(COMPONENTS, "erasure", "erasure.c"),
                os.path.join(COMPONENTS, "frag", "frag.c"),
            ],
            [
                os.path.join(COMPONENTS, "mesh_adapter", "include"),
                os.path.join(COMPONENTS, "pipeline", "include"),
                os.path.join(COMPONENTS, "erasure", "include"),
                os.path.join(COMPONENTS, "frag", "include"),
            ],
        )
        assert "8/8 passed" in out


class TestEndToEnd:
    def test_e2e_host(self):
        out = _compile_and_run_c(
            os.path.join(COMPONENTS, "mesh_adapter", "test", "test_e2e.c"),
            [
                os.path.join(COMPONENTS, "mesh_adapter", "mesh_adapter.c"),
                os.path.join(COMPONENTS, "pipeline", "pipeline.c"),
                os.path.join(COMPONENTS, "erasure", "erasure.c"),
                os.path.join(COMPONENTS, "frag", "frag.c"),
                os.path.join(COMPONENTS, "telemetry", "telemetry.c"),
            ],
            [
                os.path.join(COMPONENTS, "mesh_adapter", "include"),
                os.path.join(COMPONENTS, "pipeline", "include"),
                os.path.join(COMPONENTS, "erasure", "include"),
                os.path.join(COMPONENTS, "frag", "include"),
                os.path.join(COMPONENTS, "telemetry"),
            ],
        )
        assert "4/4 passed" in out
