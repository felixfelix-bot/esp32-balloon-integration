# ESP32-C3 BENCH console firmware (HARM-T4)

**Date:** 2026-09-16
**Task:** kanban `t_fbc3d2a1` (HARM-T4, board `e80-bench`)
**Normative spec:** `balloon-e80bench/docs/BENCH-CONSOLE-SPEC.md` v1.0 (HARM-T1)
**Depends on:** HARM-T3 (`harm/t3-radiolib-fork` — RadioLib fork with
Match123 + 511-byte FLRC, pinned as the `components/RadioLib` submodule)

---

## 1. What this is

The ESP32-C3 board (supermini + NiceRF LR2021F33 over GPSPI2) is the second of
the three bench platforms that implement the shared sweep console. This change
adds the console firmware to `tracker/firmware`:

* the E80 console trio is **vendored verbatim** (spec §11.1) — the same
  freestanding C sources the E80 and the host tooling are pinned to, so PKT
  lines and PRBS payloads are byte-comparable across boards;
* a board-independent console state machine implements every reply string, LEN
  cap, frequency window, PA cap and STAT field of the spec, and is
  **host-tested** against the E80 golden vectors;
* a thin ESP32 glue drives the patched RadioLib LR2021 (FLRC 511 B + Match123,
  LoRa) and the USB-CDC console.

No tracker/telemetry task runs in bench mode: with `CONFIG_BENCH_CONSOLE=y`
`app_main()` hands the board to the console and stops there.

## 2. Layout

| Path | Role |
|---|---|
| `tracker/firmware/components/bench/bench_cmd.c/.h` | vendored verbatim: parser + exact `ERR` strings |
| `tracker/firmware/components/bench/bench_pkt.c/.h` | vendored verbatim: 25-column `PKT` / `CONFIG_START` formatter |
| `tracker/firmware/components/bench/prbs.c/.h` | vendored verbatim: PRBS-15 fill/verify |
| `tracker/firmware/components/bench/bench_payload.c/.h` | vendored verbatim: 4-byte BE seq header + PRBS fill |
| `tracker/firmware/components/bench/bench_stats.c/.h` | vendored verbatim: PER, Wilson CI, kbps, RSSI/SNR math |
| `tracker/firmware/components/bench/buffer.c/.h` | **local shim**: `BUF_CAPACITY` for the vendored parser + spec §5 `crc16_ccitt_false` |
| `tracker/firmware/components/bench/bench_console.c/.h` | **new**: board-independent console state machine |
| `tracker/firmware/main/bench_main.cpp/.h` | **new**: RadioLib glue, FreeRTOS tasks, USB-CDC |
| `tracker/firmware/main/Kconfig.projbuild` | new `config BENCH_CONSOLE` (default n) |
| `tracker/firmware/sdkconfig.bench.defaults` | bench build defaults (`CONFIG_BENCH_CONSOLE=y`) |
| `tracker/firmware/components/bench/test/test_bench_console.c` | host suite (142 checks) |
| `tests/test_bench_host.py`, `tests/golden/*` | pytest gate + E80 golden vectors |

Vendored-file integrity: the five `.c`/`.h` pairs are md5-identical to
`balloon-e80bench` @ `f8fb0e7` (the HARM-T1 pin) — re-verify with

```bash
cd tracker/firmware/components/bench
for f in bench_cmd bench_pkt prbs bench_payload bench_stats; do
  diff -q <(cat $f.c) ~/repos/balloon-e80bench/firmware/e80-stm32-bench/src/$f.c
  diff -q include/$f.h ~/repos/balloon-e80bench/firmware/e80-stm32-bench/src/$f.h
done
```

## 3. Protocol conformance (spec §2–§10)

Board tag: **`ESP32BENCH`** (spec §10). Example `ID?` reply:

```
ID ESP32BENCH v1.0 fw=<sha7> role=RX armed=0 mod=flrc br=650000 freq=868000000 \
   band=863-870MHz pa=5 pcap=+21dBm chip=2.1 radio=awake boot=cold buf=0
```

| Spec | Status | Notes |
|---|---|---|
| §2.1 `ID?` | implemented | `band=` follows the active window (`863-870MHz` / `2400-2480MHz`); `buf=0` (no BUF staging) |
| §2.2 `ROLE TX\|RX\|NONE` | implemented | exact E80 reply strings; role change clears `armed` |
| §2.3 `ARM TX` | implemented | two-step safety gate; `ERR ROLE NOT TX` otherwise |
| §2.4 `MOD lora <sf> <bw_khz>` / `MOD flrc <br_kbps> <dbm>` | implemented | LoRa SF 5-12, BW 125/250/500 kHz; FLRC 260…2600 kbps; LoRa CR fixed 4/5 (`cr=5`), FLRC CR 3/4 (`cr=1`) |
| §2.5 `FREQ <hz>` | implemented | `ERR BAND (ESP32BENCH: 863-870MHZ OR 2400-2480MHZ ONLY)` outside the §9 sets |
| §2.6 `PA <dbm>` | implemented | indoor cap 0-10 dBm, `POWER MODE OUTDOOR <pin>` unlocks to the board cap +21 dBm |
| §2.7 `START [N=] [LEN=] [GAP=]` | implemented | TX: `OK START n=… len=… gap_us=… src=PRBS` then an async `TX DONE (RADIO ASLEEP)`; RX: `OK RX ARMED len=…` (FIX_LEN window + re-arm) |
| §2.8 `STOP` | implemented | `OK STOP (RADIO ASLEEP)` |
| §2.9 `SESSION` / `CONFIG` | implemented | `CONFIG_START,<id>,<replicate>,<ts_ms>` async marker follows `OK CONFIG` |
| §2.10 `STAT?` | implemented | spec field order; `per_ci_x1e6` only when a TX sequence was seen |
| §2.11 `PRBS ON\|OFF` | implemented | PRBS-15 verify feeds `bit_err`/`bytes_bad` |
| §2.12 `HELP` | implemented | single `CMDS:` line |
| §3 `PKT` 25 columns | implemented | emitted on RX_OK **and** CRC fail; CRC-fail rows zero `pkt_idx`/`len`/`bit_err`/`bytes_bad`/`pcrc16` but keep RSSI/SNR |
| §4 PRBS-15 payload | implemented | vendored `prbs.c` (seed `(seq ^ 0x5A5A) | 1`, MSB-first) |
| §5 `pcrc16` | implemented | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), golden `0x29B1/0xD6DA/0x0F69` |
| §6 LEN caps | implemented | parser 6…511; runtime LoRa 255 / FLRC 511 → `ERR LEN (MAX 255 LORA / 511 FLRC)` |
| §7 GAP ≥ 40 ms for LEN > 256 | host-enforced | the console does not second-guess host GAP; `docs/BENCH-CONSOLE-SPEC.md` §7 + `tools/balloon_sweep.py` enforce it |
| §8 FLRC golden RF config | implemented | Match123 + 32-bit sync `0x12AD101B` + chip CRC 2 B + CR 3/4 + preamble 32 bits + BT 1.0, re-applied before **every** burst and RX arm |
| §9 frequency plan | implemented | 863-870 MHz (default 868) and 2400-2480 MHz |
| §10 board tag | implemented | `ESP32BENCH` |
| §2.13 E80 vendor extensions | **refused by design** | `FLASH`, `BUF *`, `BAND OVERRIDE`, `PRBS9`, `OFFSET`/`CURVE`/`SYNC`/`LOADGPS` answer `ERR UNSUPPORTED (…)`. Spec §2.13: porters may omit, hosts must not require. |

Not implemented / not applicable: `QUIET ON|OFF` **is** implemented (suppresses
`PKT` lines) although the spec lists it only via the E80 command set; there is
no binary BUF staging phase, no ROM-bootloader jump and no chip-level PRBS9 TX
mode on this board.

### 3.1 Deliberate deviations from E80 (documented, spec-localised)

1. **`chip=` is reported as `2.1`.** RadioLib declares `LR2021::getVersion()`
   private with no public wrapper, so the glue reports the LR2021 revision the
   registry goldens carry (`crossboard-ids.txt`, `chip=2.1`). Verify on the
   target in HARM-T7; a public `getVersion()` in the fork would remove the
   constant.
2. **FLRC RSSI comes from `getFlrcPacketStatus()`**, i.e. chip packet RSSI
   (average + sync), matching the E80 semantics of spec §3 field 6. That getter
   is declared `private` in the fork, so `bench_main.cpp` defines
   `RADIOLIB_GODMODE (1)` for that translation unit (RadioLib's own
   access-relaxation switch; it changes access only, never layout). Follow-up:
   make the FLRC status getters public upstream and drop the define.
3. **FLRC `snr_db` is 0.** The LR2021 FLRC packet status has no SNR field.
4. **`rssi_avg_dbm` sums RX_OK events only** (divide by `rx_ok`), exactly as
   E80 `bench.c` does; CRC-fail RSSI still appears on the PKT row (spec §3).
5. **RX re-arm is `standby()` + `startReceive()` per packet**, not the E80
   continuous-RX mode: RadioLib's LR2021 module manages RX internally and the
   fork's `stageMode()` runs `CALIB_FRONT_END` on every arm. Consequence to
   watch in the HARM-T7 hardware session: on very short FLRC payloads the AGC
   may not settle, so per-packet RSSI can read low (the E80 finding that
   motivated its continuous-RX change). RX counts and PRBS/pcrc16 verification
   are unaffected.

## 4. Build

Prerequisites: ESP-IDF at `~/esp/esp-idf` (v5.4.x), `components/RadioLib`
submodule initialised at the HARM-T3 fork commit.

```bash
cd tracker/firmware
source ~/esp/esp-idf/export.sh

# (a) default = TRACKER application; bench sources compile out, image unchanged
idf.py -B build build

# (b) bench console (own sdkconfig; the tracked sdkconfig stays the tracker's)
idf.py -B build/bench \
      -DSDKCONFIG=sdkconfig.bench \
      -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench.defaults" build
```

`sdkconfig.bench` is generated and git-ignored. `sdkconfig.defaults*` only take
effect when the target sdkconfig file does not exist, which is why the bench
build uses a second file name instead of editing the tracked `sdkconfig`.

Both directions of the Kconfig gate are verified (see §5): the tracker image
must **not** contain the bench strings, the bench image must.

## 5. Verification evidence (no hardware touched)

* The host suite compiles the component with `gcc -Wall -Wextra -Werror` and
  runs all checks; `tests/test_bench_host.py` fails the build on any mismatch:

  `PYTEST_OUTPUT_PLACEHOLDER`

* Kconfig-gated smoke, both directions:

  `BUILD_EVIDENCE_PLACEHOLDER`

* On-target checks are **out of scope** for this card (no flash: the hardware
  session is HARM-T7 and needs FLASH-QUEUE approval).

### 5.1 What HARM-T7 must confirm on hardware

1. `ID?` returns `ID ESP32BENCH …`, `fw=` = the built SHA.
2. E80 TX → ESP32 RX, FLRC 650k pa5, LEN {16, 64, 255, 511} and the 2.4 GHz
   rows against RP2040: RX_OK count, `bit_err=0`, `pcrc16` present, `crc_ok=1`.
3. Reverse direction (ESP32 TX → E80/RP2040 RX).
4. RSSI/SNR sanity for FLRC (deviation 3.2 above) at bench distance.
5. `MOD lora` 868 MHz cross-board rows (SF/BW matrix) with the same `SESSION`
   id, so PKT CSVs join on `session,config,pkt_idx`.
6. `STAT?` field order parses in `tools/balloon_sweep.py` (BoardDriver).

## 6. Known issues / follow-ups

* **Residual RadioLib sync-match site.** `LR2021.cpp`
  (`setFlrcPacketParams(…, 0x01, …)` inside the TX branch of `stageMode()`)
  still re-emits `syncMatch = 0x01` on every `transmit()`. It is harmless for a
  pure TX board, but a board that transmits and then returns to RX would drop
  back to Match1-only — the cross-board breakage mode the harmonization plan
  calls out (§5). This firmware neutralises it by re-applying the golden FLRC
  config before every RX arm and burst (`apply_flrc_golden()`), but the
  underlying fork site should be patched (cached `flrcSyncMatch`) as a
  follow-up.
* The bench app does not implement E80's `QUIET`-style binary BUF staging, so
  identical-payload A/B tests must use the PRBS source.
* `TEST`/`flash` tooling: flashing is intentionally not covered here; see
  `docs/HARDWARE_MUTEX.md` in the E80 repo and the board-lock tooling.
