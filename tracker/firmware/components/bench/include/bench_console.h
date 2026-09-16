/**
 * @file    bench_console.h
 * @brief   Board-independent BENCH console state machine (ESP32-C3 board).
 *
 * Implements the reply strings, LEN caps, frequency windows, PA caps, session
 * accounting and PKT emission required by `balloon-e80bench/docs/BENCH-CONSOLE-SPEC.md`
 * v1.0 (normative) while staying free of ESP-IDF and RadioLib dependencies, so
 * the exact same code is host-tested (components/bench/test/test_bench_console.c
 * via tests/test_bench_host.py) and flashed.
 *
 * The ESP32 glue (`tracker/firmware/main/bench_main.cpp`) supplies
 * bench_radio_ops_t: RadioLib LR2021 bring-up, TX/RX, timers and the USB-CDC
 * console. Everything semantics-bearing (which reply, which cap, which CSV
 * columns) lives here.
 *
 * Board identity: `ESP32BENCH` (spec §10), sub-GHz window 863-870 MHz, 2.4 GHz
 * window 2400-2480 MHz (spec §9).
 */

#ifndef ESP32_BENCH_CONSOLE_H
#define ESP32_BENCH_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include "bench_cmd.h"
#include "bench_pkt.h"
#include "bench_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registered board tag (spec §10). */
#define BENCH_BOARD_TAG "ESP32BENCH"
/** `ID?` version field (spec §2.1). */
#define BENCH_FW_VERSION "v1.0"
/** Reply buffer size: the longest line is `STAT?` (~260 chars). */
#define BENCH_LINE_MAX 320
/** Longest console command line a host may send (spec §1: <= 96 chars). */
#define BENCH_CMD_LINE_MAX 96

#define BENCH_FREQ_DEFAULT_HZ 868000000u
#define BENCH_BAND_SUBGHZ_MIN 863000000u
#define BENCH_BAND_SUBGHZ_MAX 870000000u
#define BENCH_BAND_2G4_MIN 2400000000u
#define BENCH_BAND_2G4_MAX 2480000000u

/** Indoor TX-power cap (spec §2.4/§2.6). */
#define BENCH_TXPOW_INDOOR_CAP_DBM 10
/** Board high-frequency PA cap (spec §2.1 `pcap=`): 868 MHz and 2.4 GHz rows. */
#define BENCH_TXPOW_MAX_DBM 21
/** `pcap=` field value. */
#define BENCH_PA_CAP_LABEL "+21dBm"
/** `POWER MODE OUTDOOR <pin>` unlock key (spec §2.4 escape hatch). */
#define BENCH_OUTDOOR_PIN 2026u

/** Runtime payload caps (spec §6): LoRa silicon 255, FLRC 511. */
#define BENCH_LORA_MAX_PAYLOAD 255
#define BENCH_FLRC_MAX_PAYLOAD 511

typedef struct bench_console_s bench_console_t;

/**
 * Radio/serial side effects, implemented by the board glue.
 *
 * Every callback is optional except `apply`; a NULL callback is treated as a
 * no-op so the module stays testable with a minimal fake.
 */
typedef struct bench_radio_ops_s
{
    void *ctx;

    /** (Re)program the radio for the state's mod/freq/PA/payload length.
     *  Returns 0 on success. Called on every config change and before a
     *  burst/RX arm. */
    int (*apply)(void *ctx, const bench_console_t *st);

    /** Arm continuous RX for the configured modulation and payload window. */
    int (*rx_arm)(void *ctx, const bench_console_t *st);

    /** Send one packet synchronously. Returns 0 on success. */
    int (*tx_packet)(void *ctx, const uint8_t *buf, uint16_t len);

    /** Park the radio (spec §2.8 `OK STOP (RADIO ASLEEP)`). */
    void (*sleep)(void *ctx);

    /** Bring the radio out of sleep and back to standby. */
    void (*wake)(void *ctx);

    /** Busy-wait `us` microseconds between burst packets. */
    void (*delay_us)(void *ctx, uint32_t us);

    /** Monotonic microseconds (u64; the console derives ts_ms and elapsed_s). */
    uint64_t (*now_us)(void *ctx);

    /** LR2021 chip version for `ID? chip=<maj.min>`; 0 on success. */
    int (*chip_version)(void *ctx, uint8_t *major, uint8_t *minor);
} bench_radio_ops_t;

struct bench_console_s
{
    const bench_radio_ops_t *ops;

    /* Identity (spec §2.1) */
    const char *fw_sha;    /**< short build SHA reported as fw=<sha7> */
    const char *boot_label;/**< boot=<label>, e.g. "cold" */
    uint8_t chip_major;
    uint8_t chip_minor;

    /* Current radio configuration */
    bench_mod_t mod;      /**< BENCH_MOD_LORA | BENCH_MOD_FLRC */
    uint8_t sf;           /**< LoRa SF 5..12 (echoed on FLRC rows) */
    uint32_t bw_hz;       /**< LoRa bandwidth in Hz */
    uint32_t br_bps;      /**< FLRC bit rate in bps */
    uint8_t cr;           /**< LoRa denominator (5 = 4/5) | FLRC code (1 = 3/4) */
    int8_t txpow_dbm;
    uint32_t freq_hz;

    /* Session state */
    bench_role_t role;
    bool tx_armed;
    bool outdoor;
    bool prbs_enable;
    bool quiet;
    bool radio_awake;
    bool radio_ready;     /**< `apply` succeeded at least once */
    uint32_t drops;       /**< dropped RX events (glue increments) */

    /* Burst state */
    uint32_t tx_total;
    uint32_t tx_len;
    uint32_t tx_gap_us;
    uint32_t tx_seq;
    bool burst_active;

    /* Capture context + statistics */
    bench_pkt_ctx_t pkt_ctx;
    bench_stats_t stats;
    bool session_active;

    /* Scratch */
    uint8_t tx_buf[BENCH_FLRC_MAX_PAYLOAD];
    char reply[BENCH_LINE_MAX];
    void (*emit)(const char *line);
};

/** Initialise the console: FLRC 650 kbps / +10 dBm / 868 MHz / role NONE. */
void bench_console_init(bench_console_t *st, const bench_radio_ops_t *ops,
                        const char *fw_sha, const char *boot_label);

/** Set the sink for asynchronous lines (PKT, CONFIG_START, TX DONE) and the
 *  boot banner. The callback receives one NUL-terminated line WITHOUT the line
 *  terminator; the glue appends "\n". */
void bench_console_set_emit(bench_console_t *st, void (*emit)(const char *line));

/** Boot banner (informational; hosts ignore unrecognised lines, spec §1). */
const char *bench_console_banner(const bench_console_t *st);

/**
 * @brief Parse and execute one command line.
 *
 * @param line NUL-terminated line, no trailing CR/LF required (spec §1).
 * @return Pointer to the reply string WITHOUT the line terminator, e.g.
 *         "OK ROLE RX (CONTINUOUS)" or "ERR RANGE". Valid until the next call.
 */
const char *bench_console_handle_line(bench_console_t *st, const char *line);

/**
 * @brief Fold one received packet into the session and emit its PKT line.
 *
 * @param rssi_half_dbm RSSI in 0.5 dBm units (spec §3 field 6).
 * @param snr_qdb       SNR in 0.25 dB units, 0 for FLRC (field 7).
 * @param crc_ok        1 = chip CRC valid, 0 = CRC error (spec §3).
 * @param payload       Received payload (read only when crc_ok).
 * @param len           Received payload length (ignored when !crc_ok).
 *
 * Emits nothing unless the role is RX and QUIET is off.
 */
void bench_console_rx_event(bench_console_t *st, int16_t rssi_half_dbm,
                            int8_t snr_qdb, int crc_ok,
                            const uint8_t *payload, uint16_t len);

/** Count a dropped RX event for `STAT? ... drops=` (spec §2.10). */
void bench_console_note_drop(bench_console_t *st);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_BENCH_CONSOLE_H */
