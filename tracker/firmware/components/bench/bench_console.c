/**
 * @file    bench_console.c
 * @brief   Board-independent BENCH console state machine (spec §2-§7, §9, §10).
 *
 * Every reply string, cap and CSV column here is copied from the normative E80
 * reference (`balloon-e80bench/firmware/e80-stm32-bench/src/bench.c` handlers)
 * so that both boards produce byte-comparable captures. Deviations from E80 are
 * limited to board-specific fields the spec explicitly localises (§2.1:
 * `band=`, `pcap=`, `boot=`, `buf=`), the frequency windows of §9, and the
 * deliberately unsupported E80 vendor extensions of §2.13.
 */

#include "bench_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bench_payload.h"
#include "buffer.h"

/* --------------------------------------------------------------- utilities */

static uint64_t st_now_us(const bench_console_t *st)
{
    if (st->ops == NULL || st->ops->now_us == NULL) {
        return 0;
    }
    return st->ops->now_us(st->ops->ctx);
}

static void st_delay_us(bench_console_t *st, uint32_t us)
{
    if (st->ops != NULL && st->ops->delay_us != NULL) {
        st->ops->delay_us(st->ops->ctx, us);
    }
}

static void st_emit(bench_console_t *st, const char *line)
{
    if (st->emit != NULL) {
        st->emit(line);
    }
}

static void st_apply(bench_console_t *st)
{
    if (st->ops != NULL && st->ops->apply != NULL) {
        int rc = st->ops->apply(st->ops->ctx, st);
        if (rc == 0) {
            st->radio_ready = true;
        }
    }
}

static void st_rx_arm(bench_console_t *st)
{
    if (st->ops != NULL && st->ops->rx_arm != NULL) {
        (void)st->ops->rx_arm(st->ops->ctx, st);
    }
}

static void st_wake(bench_console_t *st)
{
    if (st->ops != NULL && st->ops->wake != NULL) {
        st->ops->wake(st->ops->ctx);
    }
    st->radio_awake = true;
}

static void st_sleep(bench_console_t *st)
{
    if (st->ops != NULL && st->ops->sleep != NULL) {
        st->ops->sleep(st->ops->ctx);
    }
    st->radio_awake = false;
}

/** snprintf-append helper: returns the new offset (never overflows). */
static int app(char *buf, int cap, int off, const char *fmt, ...)
{
    if (off < 0 || off >= cap) {
        return cap;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, (size_t)(cap - off), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return off;
    }
    return off + n;
}

static const char *role_str(const bench_console_t *st)
{
    switch (st->role) {
        case BENCH_ROLE_TX: return "TX";
        case BENCH_ROLE_RX: return "RX";
        default: return "NONE";
    }
}

/** Append a fixed-point value exactly like E80's console_put_dec1(): sign,
 *  integer part, '.', one fractional digit (value is in tenths). */
static int app_dec1(char *buf, int cap, int off, int32_t v10)
{
    int32_t mag = (v10 < 0) ? -v10 : v10;
    return app(buf, cap, off, "%s%ld.%ld", (v10 < 0) ? "-" : "",
               (long)(mag / 10), (long)(mag % 10));
}

/** spec §9: the `band=` label follows the active window. */
static const char *band_label(const bench_console_t *st)
{
    return (st->freq_hz >= 1600000000UL) ? "2400-2480MHz" : "863-870MHz";
}

static int freq_allowed(uint32_t hz)
{
    if (hz >= BENCH_BAND_SUBGHZ_MIN && hz <= BENCH_BAND_SUBGHZ_MAX) {
        return 1;
    }
    if (hz >= BENCH_BAND_2G4_MIN && hz <= BENCH_BAND_2G4_MAX) {
        return 1;
    }
    return 0;
}

static int8_t pa_cap(const bench_console_t *st)
{
    return st->outdoor ? (int8_t)BENCH_TXPOW_MAX_DBM
                       : (int8_t)BENCH_TXPOW_INDOOR_CAP_DBM;
}

/** Single PA-cap gate for `MOD flrc` and `PA` (spec §2.4/§2.6). Returns the
 *  exact E80 error reason when dbm exceeds the current cap, else NULL. */
static const char *pa_cap_err(const bench_console_t *st, int dbm)
{
    if (dbm <= pa_cap(st)) {
        return NULL;
    }
    if (pa_cap(st) == BENCH_TXPOW_INDOOR_CAP_DBM) {
        return "RANGE (INDOOR CAP 0-10 DBM; UNLOCK: POWER MODE OUTDOOR 2026)";
    }
    return "RANGE (0-21 DBM)";
}

static uint32_t max_payload(const bench_console_t *st)
{
    return (st->mod == BENCH_MOD_LORA) ? BENCH_LORA_MAX_PAYLOAD
                                       : BENCH_FLRC_MAX_PAYLOAD;
}

static const char *reply(bench_console_t *st, const char *text)
{
    snprintf(st->reply, sizeof(st->reply), "%s", text);
    return st->reply;
}

static const char *reply_err(bench_console_t *st, const char *reason)
{
    snprintf(st->reply, sizeof(st->reply), "ERR %s", reason);
    return st->reply;
}

/* ------------------------------------------------------------------- lines */

static const char *id_line(bench_console_t *st)
{
    int n = 0;
    n = app(st->reply, (int)sizeof(st->reply), n,
            "ID " BENCH_BOARD_TAG " " BENCH_FW_VERSION " fw=%s role=%s armed=%d",
            st->fw_sha ? st->fw_sha : "unknown", role_str(st), st->tx_armed ? 1 : 0);
    if (st->mod == BENCH_MOD_LORA) {
        n = app(st->reply, (int)sizeof(st->reply), n, " mod=lora sf=%u bw=%lu",
                (unsigned)st->sf, (unsigned long)st->bw_hz);
    } else {
        n = app(st->reply, (int)sizeof(st->reply), n, " mod=flrc br=%lu",
                (unsigned long)st->br_bps);
    }
    n = app(st->reply, (int)sizeof(st->reply), n,
            " freq=%lu band=%s pa=%d pcap=%s chip=%u.%u radio=%s boot=%s buf=0",
            (unsigned long)st->freq_hz, band_label(st), (int)st->txpow_dbm,
            BENCH_PA_CAP_LABEL, (unsigned)st->chip_major, (unsigned)st->chip_minor,
            st->radio_awake ? "awake" : "asleep",
            st->boot_label ? st->boot_label : "cold");
    (void)n;
    return st->reply;
}

static const char *stat_line(bench_console_t *st)
{
    uint32_t elapsed_us;
    if (st->session_active) {
        elapsed_us = bench_stats_elapsed_us(st->stats.t_start_us,
                                            (uint32_t)st_now_us(st));
    } else if (st->stats.t_stop_us != 0) {
        elapsed_us = bench_stats_elapsed_us(st->stats.t_start_us, st->stats.t_stop_us);
    } else {
        elapsed_us = 0;
    }

    uint64_t kbytes = (st->role == BENCH_ROLE_RX)
                          ? (uint64_t)st->stats.rx_bytes
                          : (uint64_t)st->stats.tx_done * (uint64_t)st->tx_len;

    int n = 0;
    n = app(st->reply, (int)sizeof(st->reply), n,
            "STAT role=%s sent=%lu sent_ok=%lu rx=%lu crc_err=%lu per_x1e6=%lu",
            role_str(st), (unsigned long)st->stats.tx_attempted,
            (unsigned long)st->stats.tx_done, (unsigned long)st->stats.rx_ok,
            (unsigned long)st->stats.rx_crc_err,
            (unsigned long)bench_stats_per_ppm(&st->stats));
    if (st->stats.rx_seq_valid) {
        /* E80 parity: the interval is reported on the PER scale, so the
         * success-rate bounds are mirrored (1000000-hi, 1000000-lo); the trial
         * count is the TX sequence span, not the received count (bench.c). */
        uint32_t lo = 0;
        uint32_t hi = 0;
        uint32_t trials = st->stats.rx_last_seq - st->stats.rx_first_seq + 1;
        bench_stats_wilson_ppm(st->stats.rx_ok, trials, &lo, &hi);
        n = app(st->reply, (int)sizeof(st->reply), n, " per_ci_x1e6=[%lu,%lu]",
                (unsigned long)(1000000UL - hi), (unsigned long)(1000000UL - lo));
    }
    n = app(st->reply, (int)sizeof(st->reply), n,
            " elapsed_s=");
    n = app_dec1(st->reply, (int)sizeof(st->reply), n, (int32_t)(elapsed_us / 100000UL));
    n = app(st->reply, (int)sizeof(st->reply), n, " kbps=%lu rssi_avg_dbm=",
            (unsigned long)bench_stats_kbps(kbytes, elapsed_us));
    n = app_dec1(st->reply, (int)sizeof(st->reply), n,
                 bench_stats_rssi_avg_half_dbm(&st->stats) * 5);
    n = app(st->reply, (int)sizeof(st->reply), n, " rssi_min_dbm=");
    n = app_dec1(st->reply, (int)sizeof(st->reply), n,
                 bench_stats_rssi_min_half_dbm(&st->stats) * 5);
    n = app(st->reply, (int)sizeof(st->reply), n, " rssi_max_dbm=");
    n = app_dec1(st->reply, (int)sizeof(st->reply), n,
                 bench_stats_rssi_max_half_dbm(&st->stats) * 5);
    n = app(st->reply, (int)sizeof(st->reply), n, " snr_avg_db=");
    n = app_dec1(st->reply, (int)sizeof(st->reply), n,
                 bench_stats_snr_avg_cdb(&st->stats) / 10);
    n = app(st->reply, (int)sizeof(st->reply), n,
            " cr=%u session=%lu config=%lu replicate=%lu drops=%lu gap_us=%lu buf=0",
            (unsigned)st->cr, (unsigned long)st->pkt_ctx.session_id,
            (unsigned long)st->pkt_ctx.config_id, (unsigned long)st->pkt_ctx.replicate,
            (unsigned long)st->drops, (unsigned long)st->tx_gap_us);
    (void)n;
    return st->reply;
}

/* ------------------------------------------------------------------- burst */

/** PRBS-sourced TX burst (spec §2.7 TX board): blocking, then parks the radio. */
static void run_burst(bench_console_t *st)
{
    st->burst_active = true;
    for (uint32_t i = 0; i < st->tx_total; i++) {
        bench_payload_build(st->tx_buf, st->tx_len, st->tx_seq);
        st->stats.tx_attempted++;
        int rc = -1;
        if (st->ops != NULL && st->ops->tx_packet != NULL) {
            rc = st->ops->tx_packet(st->ops->ctx, st->tx_buf, (uint16_t)st->tx_len);
        }
        if (rc == 0) {
            st->stats.tx_done++;
        }
        st->tx_seq++;
        if (i + 1 < st->tx_total) {
            st_delay_us(st, st->tx_gap_us);
        }
    }
    st->burst_active = false;
    st_sleep(st);
    st_emit(st, "TX DONE (RADIO ASLEEP)");
}

/* -------------------------------------------------------------- command set */

static const char *do_start(bench_console_t *st, const bench_cmd_t *c)
{
    if (st->role == BENCH_ROLE_RX) {
        /* Same line goes to both boards: on the RX board it only sets the
         * expected packet length (FLRC FIX_LEN window) and re-arms RX. */
        st->tx_len = c->len_bytes;
        bench_stats_reset(&st->stats);
        st->session_active = true;
        st_wake(st);
        st_apply(st);
        st_rx_arm(st);
        st->stats.t_start_us = (uint32_t)st_now_us(st);
        snprintf(st->reply, sizeof(st->reply), "OK RX ARMED len=%lu",
                 (unsigned long)st->tx_len);
        return st->reply;
    }
    if (st->role != BENCH_ROLE_TX) {
        return reply_err(st, "ROLE NOT TX");
    }
    if (!st->tx_armed) {
        return reply_err(st, "NOT ARMED (SEND 'ARM TX')");
    }
    if (c->len_bytes > max_payload(st)) {
        return reply_err(st, "LEN (MAX 255 LORA / 511 FLRC)");
    }

    st->tx_total = c->n_pkts;
    st->tx_len = c->len_bytes;
    st->tx_gap_us = c->gap_us;
    bench_stats_reset(&st->stats);
    st->session_active = true;
    st_wake(st);
    st_apply(st);
    st->stats.t_start_us = (uint32_t)st_now_us(st);

    /* src=BUF requires the optional BUF staging extension, which this board
     * does not implement (spec §2.13) — bursts are always PRBS-sourced. */
    snprintf(st->reply, sizeof(st->reply),
             "OK START n=%lu len=%lu gap_us=%lu src=PRBS",
             (unsigned long)st->tx_total, (unsigned long)st->tx_len,
             (unsigned long)st->tx_gap_us);
    run_burst(st);
    return st->reply;
}

const char *bench_console_handle_line(bench_console_t *st, const char *line)
{
    bench_cmd_t c;
    bench_cmd_err_t e = bench_cmd_parse(line, &c);
    if (e != BENCH_CMD_OK) {
        snprintf(st->reply, sizeof(st->reply), "ERR %s", bench_cmd_err_str(e));
        return st->reply;
    }

    switch (c.id) {
        case BENCH_CMD_ID:
            return id_line(st);

        case BENCH_CMD_ROLE:
            st->tx_armed = false;
            if (c.role == BENCH_ROLE_TX) {
                st->role = BENCH_ROLE_TX;
                st_wake(st);
                st_apply(st);
                return reply(st, "OK ROLE TX (TX INHIBITED - SEND 'ARM TX' TO ENABLE)");
            }
            if (c.role == BENCH_ROLE_RX) {
                st->role = BENCH_ROLE_RX;
                bench_stats_reset(&st->stats);
                st->session_active = true;
                st_wake(st);
                st_apply(st);
                st_rx_arm(st);
                st->stats.t_start_us = (uint32_t)st_now_us(st);
                return reply(st, "OK ROLE RX (CONTINUOUS)");
            }
            st->role = BENCH_ROLE_NONE;
            st->session_active = false;
            st_sleep(st);
            return reply(st, "OK ROLE NONE (RADIO ASLEEP)");

        case BENCH_CMD_ARM_TX:
            if (st->role != BENCH_ROLE_TX) {
                return reply_err(st, "ROLE NOT TX");
            }
            st->tx_armed = true;
            return reply(st, "OK ARMED (TX ENABLED)");

        case BENCH_CMD_MOD: {
            if (c.mod == BENCH_MOD_FLRC) {
                const char *cap_err = pa_cap_err(st, c.txpow_dbm);
                if (cap_err != NULL) {
                    return reply_err(st, cap_err);
                }
                st->mod = BENCH_MOD_FLRC;
                st->br_bps = c.br_bps;
                st->txpow_dbm = c.txpow_dbm;
                st->cr = 1; /* spec §8 golden config: FLRC CR 3/4 */
                if (st->radio_awake) {
                    st_apply(st);
                }
                snprintf(st->reply, sizeof(st->reply), "OK MOD flrc br=%lu pa=%d",
                         (unsigned long)st->br_bps, (int)st->txpow_dbm);
                return st->reply;
            }
            st->mod = BENCH_MOD_LORA;
            st->sf = c.sf;
            st->bw_hz = c.bw_hz;
            st->cr = 5; /* LoRa 4/5 (the MOD line has no CR field) */
            if (st->radio_awake) {
                st_apply(st);
            }
            snprintf(st->reply, sizeof(st->reply), "OK MOD lora sf=%u bw=%lu",
                     (unsigned)st->sf, (unsigned long)st->bw_hz);
            return st->reply;
        }

        case BENCH_CMD_FREQ:
            if (!freq_allowed(c.freq_hz)) {
                return reply_err(st, "BAND (ESP32BENCH: 863-870MHZ OR 2400-2480MHZ ONLY)");
            }
            st->freq_hz = c.freq_hz;
            if (st->radio_awake) {
                st_apply(st);
            }
            snprintf(st->reply, sizeof(st->reply), "OK FREQ %lu",
                     (unsigned long)st->freq_hz);
            return st->reply;

        case BENCH_CMD_PA:
            {
                const char *cap_err = pa_cap_err(st, c.txpow_dbm);
                if (cap_err != NULL) {
                    return reply_err(st, cap_err);
                }
            }
            st->txpow_dbm = c.txpow_dbm;
            if (st->radio_awake) {
                st_apply(st);
            }
            snprintf(st->reply, sizeof(st->reply), "OK PA %d DBM", (int)st->txpow_dbm);
            return st->reply;

        case BENCH_CMD_POWER_OUTDOOR:
            if (c.pin != BENCH_OUTDOOR_PIN) {
                return reply_err(st, "ARG (OUTDOOR UNLOCK PIN INVALID)");
            }
            st->outdoor = true;
            snprintf(st->reply, sizeof(st->reply),
                     "OK POWER MODE OUTDOOR PIN %lu ACCEPTED - TX POWER CAP LIFTED "
                     "TO +%d DBM, OUTDOOR RANGE SESSIONS ONLY",
                     (unsigned long)c.pin, (int)BENCH_TXPOW_MAX_DBM);
            return st->reply;

        case BENCH_CMD_START:
            return do_start(st, &c);

        case BENCH_CMD_STOP:
            st->stats.t_stop_us = (uint32_t)st_now_us(st);
            st->session_active = false;
            st->tx_armed = false;
            st_sleep(st);
            return reply(st, "OK STOP (RADIO ASLEEP)");

        case BENCH_CMD_STAT:
            return stat_line(st);

        case BENCH_CMD_SESSION:
            st->pkt_ctx.session_id = c.session_id;
            snprintf(st->reply, sizeof(st->reply), "OK SESSION %lu",
                     (unsigned long)st->pkt_ctx.session_id);
            return st->reply;

        case BENCH_CMD_CONFIG: {
            char marker[BENCH_LINE_MAX];
            st->pkt_ctx.config_id = c.config_id;
            st->pkt_ctx.replicate = c.replicate;
            snprintf(marker, sizeof(marker), "CONFIG_START,%lu,%lu,%lu",
                     (unsigned long)st->pkt_ctx.config_id,
                     (unsigned long)st->pkt_ctx.replicate,
                     (unsigned long)(st_now_us(st) / 1000ULL));
            snprintf(st->reply, sizeof(st->reply), "OK CONFIG %lu %lu",
                     (unsigned long)st->pkt_ctx.config_id,
                     (unsigned long)st->pkt_ctx.replicate);
            st_emit(st, marker);
            return st->reply;
        }

        case BENCH_CMD_PRBS:
            st->prbs_enable = c.prbs_enable;
            return reply(st, st->prbs_enable ? "OK PRBS ON" : "OK PRBS OFF");

        case BENCH_CMD_QUIET:
            st->quiet = c.quiet_enable;
            return reply(st, st->quiet ? "OK QUIET ON" : "OK QUIET OFF");

        case BENCH_CMD_HELP:
            return reply(st, "CMDS: ID? ROLE ARM TX MOD FREQ PA POWER MODE OUTDOOR "
                             "START STOP SESSION CONFIG STAT? PRBS QUIET HELP");

        /* E80 vendor extensions this board does not implement (spec §2.13):
         * hosts MUST NOT require them, so every one of them answers ERR. */
        case BENCH_CMD_FLASH:
            return reply_err(st, "UNSUPPORTED (STM32 ROM BOOTLOADER IS E80-ONLY)");
        case BENCH_CMD_BAND_OVERRIDE:
            return reply_err(st, "UNSUPPORTED (ESP32BENCH BAND PLAN IS FIXED)");
        case BENCH_CMD_PRBS9:
            return reply_err(st, "UNSUPPORTED (PRBS9 CHIP TX TEST MODE IS E80-ONLY)");
        case BENCH_CMD_BUF_CLEAR:
        case BENCH_CMD_BUF_LOAD:
        case BENCH_CMD_BUF_STATUS:
            return reply_err(st, "UNSUPPORTED (BUF IS AN E80 VENDOR EXTENSION)");
        case BENCH_CMD_OFFSET:
        case BENCH_CMD_CURVE:
        case BENCH_CMD_SYNC:
        case BENCH_CMD_LOADGPS:
            return reply_err(st, "UNSUPPORTED (INTERP-LOGGING COMMANDS ARE E80-ONLY)");
        case BENCH_CMD_NONE:
        default:
            return reply_err(st, "UNKNOWN");
    }
}

/* ---------------------------------------------------------------- RX events */

void bench_console_rx_event(bench_console_t *st, int16_t rssi_half_dbm,
                            int8_t snr_qdb, int crc_ok,
                            const uint8_t *payload, uint16_t len)
{
    if (st->role != BENCH_ROLE_RX) {
        return;
    }

    bench_pkt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.ts_ms = (uint32_t)(st_now_us(st) / 1000ULL);
    evt.rssi_half_dbm = rssi_half_dbm;
    evt.snr_qdb = snr_qdb;
    evt.mod = (st->mod == BENCH_MOD_LORA) ? BENCH_PKT_MOD_LORA : BENCH_PKT_MOD_FLRC;
    evt.sf = st->sf;
    evt.bw_hz = (st->mod == BENCH_MOD_LORA) ? st->bw_hz : 0;
    evt.cr = st->cr;
    evt.freq_hz = st->freq_hz;
    evt.txpow_dbm = st->txpow_dbm;

    if (crc_ok) {
        evt.len = len;
        evt.seq = bench_payload_seq(payload);
        evt.pcrc16 = crc16_ccitt_false(payload, len);
        if (st->prbs_enable) {
            uint16_t bytes_bad = 0;
            uint16_t bit_err = bench_payload_verify(payload, len, evt.seq, &bytes_bad);
            evt.bit_err = bit_err;
            evt.bytes_bad = bytes_bad;
        }
        st->stats.rx_ok++;
        st->stats.rx_bytes += len;
        st->stats.rssi_sum_half += rssi_half_dbm;
        bench_stats_note_rssi(&st->stats, rssi_half_dbm);
        st->stats.snr_sum_qdb += snr_qdb;
        if (!st->stats.rx_seq_valid) {
            st->stats.rx_first_seq = evt.seq;
            st->stats.rx_seq_valid = true;
        }
        st->stats.rx_last_seq = evt.seq;
    } else {
        /* CRC-fail row (spec §3): idx/len/bit_err/bytes_bad/pcrc16 stay 0 but
         * RSSI/SNR are still reported — never filtered from RSSI statistics. */
        st->stats.rx_crc_err++;
    }

    if (st->quiet || st->emit == NULL) {
        return;
    }

    char line[BENCH_LINE_MAX];
    bench_pkt_format(line, (int)sizeof(line), &st->pkt_ctx, &evt, crc_ok ? 1 : 0);
    st_emit(st, line);
}

void bench_console_note_drop(bench_console_t *st)
{
    st->drops++;
}

/* ------------------------------------------------------------------ startup */

void bench_console_init(bench_console_t *st, const bench_radio_ops_t *ops,
                        const char *fw_sha, const char *boot_label)
{
    memset(st, 0, sizeof(*st));
    st->ops = ops;
    st->fw_sha = fw_sha;
    st->boot_label = boot_label;

    /* Defaults per spec §9/§8: FLRC 650 kbps / 868 MHz / indoor cap, and the
     * board's hard PA cap for FREQ/PA validation. */
    st->mod = BENCH_MOD_FLRC;
    st->br_bps = 650000;
    st->cr = 1; /* FLRC 3/4 */
    st->sf = 8;
    st->bw_hz = 125000;
    st->txpow_dbm = BENCH_TXPOW_INDOOR_CAP_DBM;
    st->freq_hz = BENCH_FREQ_DEFAULT_HZ;
    st->role = BENCH_ROLE_NONE;
    bench_stats_reset(&st->stats);
    memset(&st->pkt_ctx, 0, sizeof(st->pkt_ctx));

    if (ops != NULL && ops->chip_version != NULL) {
        uint8_t maj = 0;
        uint8_t min = 0;
        if (ops->chip_version(ops->ctx, &maj, &min) == 0) {
            st->chip_major = maj;
            st->chip_minor = min;
        }
    }
}

void bench_console_set_emit(bench_console_t *st, void (*emit)(const char *line))
{
    st->emit = emit;
}

const char *bench_console_banner(const bench_console_t *st)
{
    static char banner[BENCH_LINE_MAX];
    snprintf(banner, sizeof(banner),
             "BENCH " BENCH_BOARD_TAG " " BENCH_FW_VERSION " fw=%s "
             "(spec BENCH-CONSOLE-SPEC v1.0) ready",
             st->fw_sha ? st->fw_sha : "unknown");
    return banner;
}
