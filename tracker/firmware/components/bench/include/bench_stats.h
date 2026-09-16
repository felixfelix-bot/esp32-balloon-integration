/**
 * @file    bench_stats.h
 * @brief   Portable integer statistics math for the E80 bench firmware.
 *
 * Everything is plain int32/int64 arithmetic (no floats, no libm) so the same
 * code runs in the STM32 firmware and in the host unit tests.
 */

#ifndef E80_BENCH_STATS_H
#define E80_BENCH_STATS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Rolling bench counters shared by TX and RX sides. */
typedef struct bench_stats_s
{
    uint32_t tx_attempted;  /* packets queued for TX           */
    uint32_t tx_done;       /* TX_DONE IRQs observed           */
    uint32_t rx_ok;         /* RX_DONE + CRC valid             */
    uint32_t rx_crc_err;    /* RX packets with CRC error       */
    uint32_t rx_bytes;      /* payload bytes received (ok)     */
    uint32_t rx_first_seq;  /* first TX sequence number seen   */
    uint32_t rx_last_seq;   /* last  TX sequence number seen   */
    bool     rx_seq_valid;  /* at least one sequenced packet   */
    uint32_t t_start_us;    /* session start (us timestamp)    */
    uint32_t t_stop_us;     /* session stop  (us timestamp)    */
    int32_t  rssi_sum_half; /* sum of per-pkt RSSI (0.5 dBm)   */
    int16_t  rssi_min;      /* min per-pkt RSSI (0.5 dBm)      */
    int16_t  rssi_max;      /* max per-pkt RSSI (0.5 dBm)      */
    bool     rssi_valid;    /* at least one RSSI sample        */
    int32_t  snr_sum_qdb;   /* sum of per-pkt SNR (0.25 dB)    */
} bench_stats_t;

void bench_stats_reset(bench_stats_t* s);

/**
 * @brief Packet error rate in parts-per-million of the RX session.
 *
 * Expected packets are derived from the observed TX sequence-number span
 * (last - first + 1); lost = expected - rx_ok.
 * @return ppm lost, or 0 when fewer than 2 sequenced packets were seen.
 */
uint32_t bench_stats_per_ppm(const bench_stats_t* s);

/** Wilson score 95% confidence interval for the RX success fraction.
 *  Both bounds are in ppm-of-success (0..1000000). */
void bench_stats_wilson_ppm(uint32_t successes, uint32_t trials, uint32_t* lo_ppm, uint32_t* hi_ppm);

/** Goodput in kbit/s = bytes*8000/elapsed_us (0 when elapsed==0). */
uint32_t bench_stats_kbps(uint64_t bytes, uint64_t elapsed_us);

/** Elapsed session time in us accounting for timer wrap (mod 2^32). */
uint32_t bench_stats_elapsed_us(uint32_t t_start_us, uint32_t t_stop_us);

/** Mean RSSI in 0.5 dBm units over rx_ok packets (0 when none). */
int32_t bench_stats_rssi_avg_half_dbm(const bench_stats_t* s);

/**
 * Fold one received-packet RSSI sample (0.5 dBm units, same unit as the
 * radio event rssi_half_dbm) into the min/max trackers. The first sample
 * after reset initializes both trackers.
 */
void bench_stats_note_rssi(bench_stats_t* s, int16_t rssi_half_dbm);

/** Min RSSI in 0.5 dBm units, clamped to [-256, +254] (-128.0..+127.0 dBm).
 *  Returns 0 when no sample was noted (rssi_avg convention). */
int32_t bench_stats_rssi_min_half_dbm(const bench_stats_t* s);

/** Max RSSI in 0.5 dBm units, clamped to [-256, +254] (-128.0..+127.0 dBm).
 *  Returns 0 when no sample was noted (rssi_avg convention). */
int32_t bench_stats_rssi_max_half_dbm(const bench_stats_t* s);

/** Mean SNR in centi-dB (SNR*100, i.e. 0.25 dB units * 25) over rx_ok packets. */
int32_t bench_stats_snr_avg_cdb(const bench_stats_t* s);

/** Integer square root (64-bit in, 32-bit out). Exposed for tests. */
uint32_t bench_isqrt64(uint64_t x);

#ifdef __cplusplus
}
#endif

#endif /* E80_BENCH_STATS_H */
