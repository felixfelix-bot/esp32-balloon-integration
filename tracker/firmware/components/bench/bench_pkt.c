/**
 * @file    bench_pkt.c
 * @brief   Per-packet PKT line formatter for the E80 bench firmware.
 *
 * Portable (no STM32 deps, no floats): uses snprintf to build the 24-field
 * CSV line. Compiled into both the firmware and the host unit tests.
 *
 * Field mapping:
 *   rssi_dbm  = rssi_half_dbm / 2
 *   snr_db    = snr_qdb / 4
 *   ts_ms     = evt->ts_ms (set by caller from bench_micros()/1000)
 *   mod       = "LORA" or "FLRC"
 *   bw_khz    = bw_hz / 1000
 *   cr        = evt->cr (LoRa denominator 5-8, FLRC code 0-3)
 *   GPS fields = 0 (no GPS on bench board)
 *   bit_err / bytes_bad = from PRBS-15 verification (evt fields)
 */

#include "bench_pkt.h"
#include <stdio.h>

int bench_pkt_format(char* buf, int bufsz,
                     const bench_pkt_ctx_t* ctx,
                     const bench_pkt_evt_t* evt,
                     int crc_ok)
{
    /* rssi: half-dBm -> dBm (integer division, negative values handled
     * correctly because both are signed). */
    int rssi_dbm = (evt->rssi_half_dbm) / 2;

    /* snr: quarter-dB -> dB */
    int snr_db = (evt->snr_qdb) / 4;

    /* bw_khz = bw_hz / 1000 */
    uint32_t bw_khz = evt->bw_hz / 1000;

    /* Modulation string */
    const char* mod_str = (evt->mod == BENCH_PKT_MOD_LORA) ? "LORA" : "FLRC";

    /* SF for FLRC is meaningless but emit the stored value for format
     * consistency. The host tool ignores it for FLRC. */
    uint32_t sf = evt->sf;

    /* Coding rate: emit the stored value from the event (LoRa: denominator
     * 5-8, FLRC: register code 0-3). The host tooling interprets it. */
    uint32_t cr = evt->cr;

    /* Timestamp from the event (caller sets bench_micros()/1000). */
    uint32_t ts_ms = evt->ts_ms;

    /* PKT,<session>,<config>,<replicate>,<seq>,<ts_ms>,<rssi>,<snr>,
     * <crc_ok>,<bit_err>,<bytes_bad>,<freq_hz>,<mod>,<sf>,<bw_khz>,
     * <cr>,<power>,<pkt_size>,<gps_fix>,<gps_lat>,<gps_lon>,<gps_alt>,
     * <gps_sats>,<gps_hdop>,<pcrc16>
     */
    int n = snprintf(buf, (size_t)bufsz,
        "PKT,%lu,%lu,%lu,%lu,%lu,%d,%d,%d,%u,%u,%lu,%s,%lu,%lu,%lu,%d,%u,0,0,0,0,0,0,%u",
        (unsigned long)ctx->session_id,
        (unsigned long)ctx->config_id,
        (unsigned long)ctx->replicate,
        (unsigned long)evt->seq,
        (unsigned long)ts_ms,
        rssi_dbm,
        snr_db,
        crc_ok,
        (unsigned)evt->bit_err,
        (unsigned)evt->bytes_bad,
        (unsigned long)evt->freq_hz,
        mod_str,
        (unsigned long)sf,
        (unsigned long)bw_khz,
        (unsigned long)cr,
        (int)evt->txpow_dbm,
        (unsigned)evt->len,
        (unsigned)evt->pcrc16);

    /* snprintf returns the number of chars that WOULD have been written.
     * If n >= bufsz the output was truncated but buf is NUL-terminated. */
    return n;
}

int bench_pkt_config_start(char* buf, int bufsz,
                           const bench_pkt_ctx_t* ctx,
                           uint32_t ts_ms)
{
    /* CONFIG_START,<config_id>,<replicate>,<ts_ms>
     * (E80-8/O4: transition marker for host-side capture segmentation) */
    int n = snprintf(buf, (size_t)bufsz,
        "CONFIG_START,%lu,%lu,%lu",
        (unsigned long)ctx->config_id,
        (unsigned long)ctx->replicate,
        (unsigned long)ts_ms);

    return n;
}