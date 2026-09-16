/**
 * @file    buffer.c
 * @brief   CRC-16/CCITT-FALSE (spec §5) — the only CRC implementation in the
 *          ESP32-C3 bench component.
 *
 * The bitwise body is copied from the normative E80 implementation
 * (balloon-e80bench firmware/e80-stm32-bench/src/buffer.c) so both boards emit
 * identical `pcrc16` values for identical payloads. ~40 B of code, no table.
 *
 * Golden vectors (spec §5, shared with the E80 host tool
 * tools/test_crc16_golden.py):
 *   "123456789"       -> 0x29B1
 *   64 x 0x00         -> 0xD6DA
 *   4096 x (i % 256)  -> 0x0F69
 */

#include "buffer.h"

uint16_t crc16_ccitt_false(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}
