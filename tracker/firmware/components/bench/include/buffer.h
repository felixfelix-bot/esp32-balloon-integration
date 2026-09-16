/**
 * @file    buffer.h
 * @brief   ESP32-C3 bench firmware shim for the vendored E80 `bench_cmd.c`.
 *
 * `balloon-e80bench/firmware/e80-stm32-bench/src/bench_cmd.c` is ported
 * VERBATIM (spec §11.1) and includes "buffer.h" for one macro: the
 * `BUF LOAD <n>` parse range check. The BUF subsystem itself is an E80 vendor
 * extension the spec makes optional (§2.13: "Porters MAY implement or omit;
 * hosts MUST NOT require them"), so this shim supplies only what the vendored
 * parser needs plus the CRC-16 reference implementation of spec §5 — it is the
 * one file in this component that is NOT a verbatim copy.
 *
 * ESP32 bench firmware: BUF staging is not implemented; every BUF command is
 * answered `ERR UNSUPPORTED (BUF IS AN E80 VENDOR EXTENSION)` and `ID?`
 * reports `buf=0`.
 */

#ifndef ESP32_BENCH_BUFFER_H
#define ESP32_BENCH_BUFFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound accepted by the vendored `BUF LOAD <n> <crc16>` parser
 * (E80: the 4096-byte TX payload arena). */
#define BUF_CAPACITY 4096u

/* E80 binary-phase idle timeout (unused here: no binary phase on ESP32). */
#define BUF_IDLE_TIMEOUT_MS 1000u

/**
 * @brief CRC-16/CCITT-FALSE over data[0..len-1].
 *
 * poly 0x1021, init 0xFFFF, MSB-first, no input/output reflection, no final
 * XOR — spec §5, bitwise implementation copied from E80 buffer.c.
 * Golden vectors: "123456789" -> 0x29B1, 64 x 0x00 -> 0xD6DA,
 * 4096 x (i % 256) -> 0x0F69.
 */
uint16_t crc16_ccitt_false(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_BENCH_BUFFER_H */
