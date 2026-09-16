# ESP32-C3 BENCH console component (HARM-T4).
#
# Vendored verbatim from balloon-e80bench/firmware/e80-stm32-bench/src (the
# normative reference, docs/BENCH-CONSOLE-SPEC.md §11.1):
#   bench_cmd.c/h    command parser + exact ERR strings
#   bench_pkt.c/h    PKT / CONFIG_START line formatter (25 columns)
#   prbs.c/h         PRBS-15 fill + verify
#   bench_payload.c/h 4-byte BE sequence header + PRBS fill
#   bench_stats.c/h  PER / Wilson CI / kbps / RSSI-SNR integer math
#
# Local (not vendored):
#   buffer.c/h       BUF_CAPACITY shim for the vendored parser + the spec §5
#                    CRC-16/CCITT-FALSE reference implementation
#   bench_console.c/h board-independent console state machine (replies, LEN
#                    caps, frequency windows, STAT accounting, PKT emission)
#
# The ESP32-specific RadioLib glue lives in main/bench_main.cpp; this component
# is deliberately free of ESP-IDF and RadioLib dependencies so it compiles on
# the host (see test/test_bench_console.c and tests/test_bench_host.py).
idf_component_register(
    SRCS
        "bench_cmd.c"
        "bench_pkt.c"
        "prbs.c"
        "bench_payload.c"
        "bench_stats.c"
        "buffer.c"
        "bench_console.c"
    INCLUDE_DIRS "include"
    REQUIRES
)
