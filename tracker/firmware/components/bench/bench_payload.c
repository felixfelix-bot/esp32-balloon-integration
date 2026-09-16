/**
 * @file    bench_payload.c
 * @brief   Bench payload generator: 4-byte BE seq header + PRBS15 fill.
 */

#include "bench_payload.h"
#include "prbs.h"

static void put_u32be(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static uint32_t get_u32be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

void bench_payload_build(uint8_t* buf, uint32_t len, uint32_t seq)
{
    put_u32be(&buf[0], seq);

    /* PRBS15 fill after the 4-byte header, seeded from the seq number */
    if (len > BENCH_PAYLOAD_HDR_LEN)
        prbs15_fill(buf + BENCH_PAYLOAD_HDR_LEN, len - BENCH_PAYLOAD_HDR_LEN, seq);
}

uint32_t bench_payload_seq(const uint8_t* buf)
{
    return get_u32be(buf);
}

uint16_t bench_payload_verify(const uint8_t* buf, uint32_t len, uint32_t seq, uint16_t* out_bytes_bad)
{
    if (len < BENCH_PAYLOAD_HDR_LEN)
    {
        if (out_bytes_bad) *out_bytes_bad = 0;
        return 0;
    }

    return prbs15_verify(buf + BENCH_PAYLOAD_HDR_LEN,
                         len - BENCH_PAYLOAD_HDR_LEN,
                         seq, out_bytes_bad);
}