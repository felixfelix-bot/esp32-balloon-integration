#ifndef PRBS_H
#define PRBS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill buf with len bytes of PRBS15 LFSR output.
 *
 * Polynomial: x^15 + x^14 + 1 (Galois LFSR, taps at bits 14 and 13).
 * Seed: (uint16_t)(seed ^ 0x5A5A) | 1.
 * Matches the C3 reference algorithm for cross-rig compatibility.
 */
void prbs15_fill(uint8_t *buf, size_t len, uint32_t seed);

/**
 * @brief Verify buf against the expected PRBS15 stream.
 *
 * @param buf          Payload bytes to verify.
 * @param len          Number of bytes.
 * @param seed         Seed value (same as used for fill).
 * @param out_bytes_bad Written with count of bytes that differ (may be NULL).
 * @return Total bit errors (sum of popcount of XOR diff per byte).
 */
uint16_t prbs15_verify(const uint8_t *buf, size_t len, uint32_t seed, uint16_t *out_bytes_bad);

#ifdef __cplusplus
}
#endif

#endif /* PRBS_H */