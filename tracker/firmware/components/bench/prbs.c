/**
 * @file    prbs.c
 * @brief   PRBS15 fill + verify — Galois LFSR, C3-compatible.
 *
 * Polynomial: x^15 + x^14 + 1 (taps at bits 14 and 13).
 * Seed: (uint16_t)(seed ^ 0x5A5A) | 1.
 * MSB-first byte assembly.
 * Verify: regenerate + XOR + __builtin_popcount for bit count.
 */

#include "prbs.h"

void prbs15_fill(uint8_t *buf, size_t len, uint32_t seed)
{
    uint16_t state = (uint16_t)(seed ^ 0x5A5A) | 1;

    for (size_t i = 0; i < len; i++)
    {
        uint8_t byte_val = 0;
        for (int b = 0; b < 8; b++)
        {
            uint16_t newbit = ((state >> 14) ^ (state >> 13)) & 1;
            state = ((state << 1) | newbit) & 0x7FFF;
            byte_val = (byte_val << 1) | (newbit & 1);
        }
        buf[i] = byte_val;
    }
}

uint16_t prbs15_verify(const uint8_t *buf, size_t len, uint32_t seed, uint16_t *out_bytes_bad)
{
    uint16_t state = (uint16_t)(seed ^ 0x5A5A) | 1;
    uint16_t bit_errors = 0;
    uint16_t bytes_bad = 0;

    for (size_t i = 0; i < len; i++)
    {
        uint8_t expected = 0;
        for (int b = 0; b < 8; b++)
        {
            uint16_t newbit = ((state >> 14) ^ (state >> 13)) & 1;
            state = ((state << 1) | newbit) & 0x7FFF;
            expected = (expected << 1) | (newbit & 1);
        }
        uint8_t diff = buf[i] ^ expected;
        if (diff)
        {
            bytes_bad++;
            bit_errors += __builtin_popcount(diff);
        }
    }

    if (out_bytes_bad)
        *out_bytes_bad = bytes_bad;
    return bit_errors;
}