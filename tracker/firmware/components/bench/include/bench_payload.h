/**
 * @file    bench_payload.h
 * @brief   Portable bench payload generator: TX sequence header + PRBS15 fill.
 *
 * Payload layout (big-endian, C3-compatible):
 *   [0..3] u32 tx sequence number, BIG-ENDIAN (buf[0]=MSB, buf[3]=LSB)
 *   [4..N] PRBS15 fill (seed = seq number), N = pkt_size - 4
 *
 * No length field in header (C3 harmonization).
 */

#ifndef E80_BENCH_PAYLOAD_H
#define E80_BENCH_PAYLOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_PAYLOAD_HDR_LEN 4

/** Build a bench payload for sequence number seq into buf[0..len-1].
 *  Requires len >= BENCH_PAYLOAD_HDR_LEN; len <= 511 by protocol. */
void bench_payload_build(uint8_t* buf, uint32_t len, uint32_t seq);

/** Extract the TX sequence number from a received payload (big-endian). */
uint32_t bench_payload_seq(const uint8_t* buf);

/** Read the current TX sequence counter (host + fw). */
uint32_t bench_get_tx_seq(void);

/** Verify the PRBS15 fill of a received payload.
 *  @param buf       Payload buffer (header + body).
 *  @param len       Total payload length in bytes.
 *  @param seq       Sequence number (used as PRBS15 seed).
 *  @param out_bytes_bad  If non-NULL, receives count of mismatched bytes.
 *  @return Number of bit errors (0 = perfect match). */
uint16_t bench_payload_verify(const uint8_t* buf, uint32_t len, uint32_t seq, uint16_t* out_bytes_bad);

#ifdef __cplusplus
}
#endif

#endif /* E80_BENCH_PAYLOAD_H */