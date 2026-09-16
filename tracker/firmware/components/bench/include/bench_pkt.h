/**
 * @file    bench_pkt.h
 * @brief   Per-packet PKT line formatter for the E80 bench firmware.
 *
 * Emits one CSV line per received packet (RX_OK and RX_CRC) in the
 * 24-field format consumed by the host-side analysis tooling:
 *
 *   PKT,<session_id>,<config_id>,<replicate>,<seq>,<ts_ms>,<rssi_dbm>,
 *   <snr_db>,<crc_ok>,<bit_err>,<bytes_bad>,<freq_hz>,<mod>,<sf>,
 *   <bw_khz>,<cr>,<power_dbm>,<pkt_size>,<gps_fix>,<gps_lat>,<gps_lon>,
 *   <gps_alt>,<gps_sats>,<gps_hdop>,<pcrc16>
 *
 * Field 24 (pcrc16): CRC-16/CCITT-FALSE over the received payload bytes
 * (radio_bench_rx_buf[0..len-1]). 0 for CRC-failed packets (no payload
 * read). APPEND ONLY — never reorder existing fields (BUF-T5a).
 *
 * Portable (no STM32 deps, no floats): compiled into both the firmware
 * and the host unit tests.
 */

#ifndef E80_BENCH_PKT_H
#define E80_BENCH_PKT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Session context for PKT lines (set by SESSION / CONFIG commands). */
typedef struct bench_pkt_ctx_s
{
    uint32_t session_id;   /* SESSION <id>  (0 default) */
    uint32_t config_id;    /* CONFIG <id> <replicate>   */
    uint32_t replicate;    /* CONFIG <id> <replicate>   */
} bench_pkt_ctx_t;

/** Modulation type for the PKT line. */
typedef enum bench_pkt_mod_e
{
    BENCH_PKT_MOD_LORA = 0,
    BENCH_PKT_MOD_FLRC,
} bench_pkt_mod_t;

/** Per-packet event data extracted from the radio event + config.
 *  This is a portable subset of rb_evt_t + radio_bench_cfg_t so that
 *  bench_pkt.c compiles on host without STM32/LR2021 headers. */
typedef struct bench_pkt_evt_s
{
    uint32_t      seq;            /* TX sequence number             */
    uint16_t      len;            /* payload length in bytes        */
    int16_t       rssi_half_dbm;  /* RSSI in 0.5 dBm units (dBm*2)  */
    int8_t        snr_qdb;         /* SNR in 0.25 dB units           */
    bench_pkt_mod_t mod;           /* LORA or FLRC                  */
    uint8_t       sf;             /* LoRa SF 5..12                  */
    uint32_t      bw_hz;          /* LoRa bandwidth in Hz           */
    uint32_t      freq_hz;        /* RF frequency                   */
    int8_t        txpow_dbm;      /* TX power setting               */
    uint8_t       cr;             /* Coding rate (LoRa denom 5-8, FLRC code 0-3) */
    uint32_t      ts_ms;          /* Timestamp in milliseconds      */
    uint16_t      bit_err;        /* PRBS15 bit error count         */
    uint16_t      bytes_bad;      /* PRBS15 mismatched byte count   */
    uint16_t      pcrc16;         /* CRC-16/CCITT-FALSE over RX payload (BUF-T5a) */
} bench_pkt_evt_t;

/**
 * @brief Format a PKT line into buf.
 *
 * @param buf      Output buffer (NUL-terminated on return).
 * @param bufsz    Size of buf in bytes.
 * @param ctx      Session/config context (session_id, config_id, replicate).
 * @param evt      Packet data (seq, len, rssi, snr, mod, sf, bw, freq, txpow, cr, ts_ms).
 * @param crc_ok   1 = CRC valid (RX_OK), 0 = CRC failed (RX_CRC).
 * @return Number of bytes that would be written (excluding NUL).
 *         If >= bufsz, the output was truncated but buf is NUL-terminated.
 */
int bench_pkt_format(char* buf, int bufsz,
                     const bench_pkt_ctx_t* ctx,
                     const bench_pkt_evt_t* evt,
                     int crc_ok);

/**
 * @brief Format a CONFIG_START marker line.
 *
 * Emitted when the firmware receives a CONFIG command, allowing the
 * host capture tool to segment captures by configuration window.
 *
 * @param buf      Output buffer (NUL-terminated on return).
 * @param bufsz    Size of buf in bytes.
 * @param ctx      Session/config context (uses config_id and replicate).
 * @param ts_ms    Timestamp in milliseconds.
 * @return Number of bytes that would be written (excluding NUL).
 */
int bench_pkt_config_start(char* buf, int bufsz,
                           const bench_pkt_ctx_t* ctx,
                           uint32_t ts_ms);

#ifdef __cplusplus
}
#endif

#endif /* E80_BENCH_PKT_H */