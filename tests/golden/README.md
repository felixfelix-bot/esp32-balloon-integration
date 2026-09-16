# Golden vectors — provenance

These files are **copies** of the normative E80 golden vectors. They are the
cross-board oracle for the ESP32-C3 bench firmware (HARM-T4) and are read
verbatim by the host test `components/bench/test/test_bench_console.c` (driven
by `tests/test_bench_host.py`).

| File | Copied from | Purpose |
|---|---|---|
| `e80-flrc-session.txt` | `balloon-e80bench/tests/golden/e80-flrc-session.txt` | 25-column `PKT` lines (RX_OK **and** CRC-fail rows) + `STAT` line — byte-exact `bench_pkt_format()` parity target |
| `e80-lora-24col.txt` | `balloon-e80bench/tests/golden/e80-lora-24col.txt` | 24-column (pre-BUF-T5a) `PKT` lines — append-only tolerance target |
| `e80-stat-recorded.txt` | `balloon-e80bench/tests/golden/e80-stat-recorded.txt` | Recorded `STAT` lines (field-set reference) |
| `crossboard-ids.txt` | `balloon-e80bench/tests/golden/crossboard-ids.txt` | Registered board tags `ESP32BENCH`/`RP2040BENCH`/`E80BENCH` + `ID?` field-set reference |

Source revision: `balloon-e80bench` @ `f8fb0e7a32f11c7c52e23874fe0d9d288f20b886`
(2026-08-21), the commit HARM-T1 pinned when it froze
`docs/BENCH-CONSOLE-SPEC.md` v1.0 (§4, §5, §3).

Do **not** edit these files to make a test pass: a mismatch is a parity bug in
the firmware under test. Regenerate only by re-copying from the E80 repo.
