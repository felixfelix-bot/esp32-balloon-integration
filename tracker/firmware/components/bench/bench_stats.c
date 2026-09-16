/**
 * @file    bench_stats.c
 * @brief   Portable integer statistics math (PER, Wilson 95% CI, kbps).
 */

#include "bench_stats.h"

#include <stddef.h>

void bench_stats_reset(bench_stats_t* s)
{
    s->tx_attempted = 0;
    s->tx_done = 0;
    s->rx_ok = 0;
    s->rx_crc_err = 0;
    s->rx_bytes = 0;
    s->rx_first_seq = 0;
    s->rx_last_seq = 0;
    s->rx_seq_valid = false;
    s->t_start_us = 0;
    s->t_stop_us = 0;
    s->rssi_sum_half = 0;
    s->rssi_min = 0;
    s->rssi_max = 0;
    s->rssi_valid = false;
    s->snr_sum_qdb = 0;
}

uint32_t bench_isqrt64(uint64_t x)
{
    if (x == 0)
        return 0;
    /* Classic bit-by-bit integer sqrt: exact floor(sqrt(x)). */
    uint64_t res = 0;
    uint64_t bit = 1ULL << 62; /* highest power of 4 <= 2^64 */
    while (bit > x)
        bit >>= 2;
    while (bit != 0)
    {
        if (x >= res + bit)
        {
            x -= res + bit;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

uint32_t bench_stats_per_ppm(const bench_stats_t* s)
{
    if (!s->rx_seq_valid)
        return 0;
    uint32_t expected = s->rx_last_seq - s->rx_first_seq + 1;
    if (expected == 0 || s->rx_ok > expected)
        return 0;
    uint64_t lost = (uint64_t)(expected - s->rx_ok);
    return (uint32_t)((lost * 1000000ULL) / expected);
}

void bench_stats_wilson_ppm(uint32_t successes, uint32_t trials, uint32_t* lo_ppm, uint32_t* hi_ppm)
{
    /* Wilson score interval, z = 1.96 (95%), integer math scaled by 1e6.
     *
     * Working with counts S (successes), N (trials):
     *   center = (S + z^2/2) / (N + z^2)
     *   half   = z * sqrt( S(N-S)/N + z^2/4 ) / (N + z^2)
     *   lo/hi  = center -/+ half, clamped to [0, 1]
     *
     * Single shared denominator den = (N + z^2) * 1e6; both numerators
     * are kept undivided and rounded to nearest, so S == N yields
     * exactly 1,000,000 ppm (the exact value) instead of 999,987+ after
     * floor truncation.  z = 1.96 -> Z1e6 = 1,960,000; z^2 -> Z2e6 = 3,841,600. */
    const uint64_t Z1e6 = 1960000ULL;
    const uint64_t Z2e6 = 3841600ULL;

    if (trials == 0)
    {
        *lo_ppm = 0;
        *hi_ppm = 1000000;
        return;
    }

    uint64_t S = successes;
    uint64_t N = trials;

    uint64_t den = N * 1000000ULL + Z2e6;
    uint64_t center_num = (S * 1000000ULL + Z2e6 / 2) * 1000000ULL;

    /* inner_num = (S(N-S)/N + z^2/4) * 1e6;  r = sqrt(inner_num) ~ sqrt(inner)*1e3 */
    uint64_t inner_num = (S * (N - S)) * 1000000ULL / N + Z2e6 / 4;
    uint64_t r = bench_isqrt64(inner_num);
    uint64_t half_num = r * Z1e6 * 1000ULL;

    uint64_t lo_num = center_num + den / 2 >= half_num ? center_num + den / 2 - half_num : 0;
    uint64_t hi_num = center_num + half_num + den / 2;

    *lo_ppm = (lo_num / den > 1000000ULL) ? 0 : (uint32_t)(lo_num / den);
    *hi_ppm = (hi_num / den >= 1000000ULL) ? 1000000 : (uint32_t)(hi_num / den);
}

uint32_t bench_stats_kbps(uint64_t bytes, uint64_t elapsed_us)
{
    if (elapsed_us == 0)
        return 0;
    /* kbit/s = bytes * 8 / (elapsed_us / 1e6) / 1000 = bytes * 8000 / elapsed_us */
    return (uint32_t)((bytes * 8000ULL) / elapsed_us);
}

uint32_t bench_stats_elapsed_us(uint32_t t_start_us, uint32_t t_stop_us)
{
    return t_stop_us - t_start_us; /* mod-2^32 arithmetic handles wrap */
}

int32_t bench_stats_rssi_avg_half_dbm(const bench_stats_t* s)
{
    if (s->rx_ok == 0)
        return 0;
    return s->rssi_sum_half / (int32_t)s->rx_ok;
}

void bench_stats_note_rssi(bench_stats_t* s, int16_t rssi_half_dbm)
{
    if (!s->rssi_valid)
    {
        s->rssi_min = rssi_half_dbm;
        s->rssi_max = rssi_half_dbm;
        s->rssi_valid = true;
        return;
    }
    if (rssi_half_dbm < s->rssi_min)
        s->rssi_min = rssi_half_dbm;
    if (rssi_half_dbm > s->rssi_max)
        s->rssi_max = rssi_half_dbm;
}

/* Trackers store raw samples; the printable range is clamped here so the
 * STAT line can never emit an RSSI outside -128.0..+127.0 dBm (int8 dBm). */
#define RSSI_HALF_MIN (-256) /* -128.0 dBm */
#define RSSI_HALF_MAX (254)  /* +127.0 dBm */

int32_t bench_stats_rssi_min_half_dbm(const bench_stats_t* s)
{
    if (!s->rssi_valid)
        return 0;
    if (s->rssi_min < RSSI_HALF_MIN)
        return RSSI_HALF_MIN;
    if (s->rssi_min > RSSI_HALF_MAX)
        return RSSI_HALF_MAX;
    return s->rssi_min;
}

int32_t bench_stats_rssi_max_half_dbm(const bench_stats_t* s)
{
    if (!s->rssi_valid)
        return 0;
    if (s->rssi_max < RSSI_HALF_MIN)
        return RSSI_HALF_MIN;
    if (s->rssi_max > RSSI_HALF_MAX)
        return RSSI_HALF_MAX;
    return s->rssi_max;
}

int32_t bench_stats_snr_avg_cdb(const bench_stats_t* s)
{
    if (s->rx_ok == 0)
        return 0;
    /* snr_sum_qdb is in 0.25 dB units; *25 -> centi-dB. */
    return (s->snr_sum_qdb * 25) / (int32_t)s->rx_ok;
}
