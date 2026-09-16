// TDD RED test: LR2021 FLRC Match123 + 511-byte payload configuration.
//
// Golden SPI bytes come from the range-test-proven balloon firmware
// (multi_radio_sweep_rx_v4.cpp, SET_FLRC_PACKET_PARAMS 0x0249):
//
//   {0x02, 0x49, 0x0E, 0x7C, 0x01, 0xFF}
//
//   byte0 0x0E = agc_pbl_len 3 (16-bit preamble) << 2 | sw_len 4/2 (32-bit sync)
//   byte1 0x7C = swTx 01 | sw_match 111 (Match123) | fixed 1 | crc 00
//                (stock RadioLib emits 0x4C = sw_match 001 (Match1) - the exact
//                 "was 0x4C" broken value the prior-art firmware fixed)
//   byte2..3   = payload length, 511 -> {0x01, 0xFF}
//
// The configuration MUST be reachable through the public API and MUST survive
// re-emission from cached state (setPreambleLength/setCRC/setPacketMode/
// stageMode all re-send FLRC packet params).
//
// RED phase: this file fails to COMPILE until the patch adds
//   - LR2021::setFlrcSyncWordMatch(uint8_t) (public setter caching the match)
//   - RADIOLIB_LR2021_FLRC_SYNC_MATCH_1_2_3 constant
//   - FLRC payload length > 255 (fixedPacketLengthMode(511) must not truncate)
// GREEN phase: every check below must pass with the golden bytes.
#include <cstdio>
#include <string>
#include <vector>

#include "mock_lr2021_hal.h"
#include "LR2021.h"
#include "LR2021_commands.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
  if(!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while(0)

static std::string frameHex(const std::vector<uint8_t>& f) {
  return(MockLR2021Hal::hex(f));
}

int main() {
  MockLR2021Hal hal;
  // the only firmware version mentioned in the LR2021 datasheet
  hal.canned[0x0101] = { 0x01, 0x18 };  // GET_VERSION
  hal.canned[0x0208] = { 0x05 };        // GET_PACKET_TYPE -> FLRC

  // pins are fully mocked; values are arbitrary
  Module mod(&hal, 10 /* cs */, 11 /* irq */, 12 /* rst */, 13 /* gpio */);
  LR2021 radio(&mod);

  // HF PA is limited to +12 dBm on the LR2021
  int16_t st = radio.beginFLRC(2478.0f, 260.0f, RADIOLIB_LR2021_FLRC_CR_3_4, 12, 16, RADIOLIB_SHAPING_0_5);
  if(st != RADIOLIB_ERR_NONE) {
    printf("beginFLRC failed with code %d\n", (int)st);
  }
  CHECK(st == RADIOLIB_ERR_NONE, "beginFLRC failed");

  // apply the range-test-proven configuration through the public API:
  // CRC field 0 + Match123 + fixed-length 511-byte payloads
  st = radio.setCRC(0);
  CHECK(st == RADIOLIB_ERR_NONE, "setCRC(0) failed");

  st = radio.setFlrcSyncWordMatch(RADIOLIB_LR2021_FLRC_SYNC_MATCH_1_2_3);
  CHECK(st == RADIOLIB_ERR_NONE, "setFlrcSyncWordMatch(Match123) failed");

  st = radio.fixedPacketLengthMode(511);
  CHECK(st == RADIOLIB_ERR_NONE, "fixedPacketLengthMode(511) failed");

  // ---- check 1: emission equals the golden frame ----
  auto frames = hal.findCmd(0x0249);  // SET_FLRC_PACKET_PARAMS
  CHECK(!frames.empty(), "no SET_FLRC_PACKET_PARAMS frame captured");
  if(!frames.empty()) {
    // write frame layout: [cmdHi, cmdLo, statusPad, statusPad, b0, b1, b2, b3]
    const std::vector<uint8_t> golden = { 0x0E, 0x7C, 0x01, 0xFF };
    std::vector<uint8_t> params(frames.back().end() - 4, frames.back().end());
    printf("direct  0x0249 params: %s (golden: 0E 7C 01 FF)\n", frameHex(params).c_str());
    CHECK(params == golden, "configured packet params do not emit golden bytes");
  }

  // ---- check 2: cached re-emission must preserve Match123 + 511 ----
  const size_t countBefore = frames.size();
  st = radio.setPreambleLength(16);
  CHECK(st == RADIOLIB_ERR_NONE, "setPreambleLength(16) failed");
  frames = hal.findCmd(0x0249);
  CHECK(frames.size() == countBefore + 1, "setPreambleLength did not re-emit FLRC packet params");
  if(frames.size() == countBefore + 1) {
    const std::vector<uint8_t> golden = { 0x0E, 0x7C, 0x01, 0xFF };
    std::vector<uint8_t> params(frames.back().end() - 4, frames.back().end());
    printf("cached  0x0249 params: %s (golden: 0E 7C 01 FF)\n", frameHex(params).c_str());
    if(params != golden) {
      printf("FAIL: cached re-emission reverted Match123/511 config: got %s want 0E 7C 01 FF\n",
             frameHex(params).c_str());
      failures++;
    }
  }

  // ---- check 3: RX staging selects the HF path for 2.4 GHz ----
  // stageMode() auto-emits SET_RX_PATH from the configured frequency:
  //   path byte = RADIOLIB_LR2021_RX_PATH_HF (0x01) - the golden 2.4G value
  //   boost byte = RADIOLIB_LR2021_RX_BOOST_HF (0x04) - RadioLib default;
  //                prior-art golden frame used 0x00 (boost off) - deliberate
  //                divergence, kept: boost only trades current for sensitivity
  st = radio.startReceive();
  CHECK(st == RADIOLIB_ERR_NONE, "startReceive failed");
  auto rxPaths = hal.findCmd(0x0201);  // SET_RX_PATH
  CHECK(!rxPaths.empty(), "no SET_RX_PATH frame captured during RX staging");
  if(!rxPaths.empty()) {
    // write frame: [cmdHi, cmdLo, statusPad, statusPad, path, boost]
    std::vector<uint8_t> params(rxPaths.back().end() - 2, rxPaths.back().end());
    printf("rxpath  0x0201 params: %s (golden path byte: 01, boost: 04 RadioLib / 00 golden)\n",
           frameHex(params).c_str());
    CHECK(params[0] == 0x01, "RX staging did not select the HF (2.4 GHz) path");
  }

  // ---- check 4: TX staging must re-emit the cached sync-match (FIX-T3.1) ----
  // transmit() -> setMode(RADIOLIB_RADIO_MODE_TX) -> stageMode() used to pass a
  // hard-coded syncMatch = 0x01 (Match1), so a board that transmitted and then
  // returned to RX silently fell back to Match1-only reception while its peer
  // still sent Match123 traffic. Every SET_FLRC_PACKET_PARAMS emitted by a
  // configured radio must therefore carry the CACHED match value.
  const size_t paramsBeforeTx = hal.findCmd(0x0249).size();
  std::vector<uint8_t> txPayload(511, 0xA5);  // fixed-length 511-byte FLRC payload
  st = radio.transmit(txPayload.data(), txPayload.size());
  // The mocked IRQ line never asserts, so transmit() always ends in its own
  // timeout - the SPI frames emitted while STAGING the TX are the subject here.
  CHECK((st == RADIOLIB_ERR_NONE) || (st == RADIOLIB_ERR_TX_TIMEOUT),
        "transmit() returned an unexpected error code");
  {
    size_t paramsSeen = 0;
    size_t paramsAfterTx = 0;
    for(const auto& f : hal.frames) {
      if((f.size() < 4) || ((((uint16_t)f[0] << 8) | f[1]) != 0x0249)) { continue; }
      paramsSeen++;
      if(paramsSeen <= paramsBeforeTx) { continue; }  // checked by checks 1-2
      paramsAfterTx++;
      const std::vector<uint8_t> params(f.end() - 4, f.end());
      printf("tx      0x0249 params: %s (golden: 0E 7C 01 FF)\n", frameHex(params).c_str());
      CHECK(params == std::vector<uint8_t>({ 0x0E, 0x7C, 0x01, 0xFF }),
            "TX staging re-emitted a stale sync-match / payload length");
    }
    CHECK(paramsAfterTx > 0, "transmit() emitted no SET_FLRC_PACKET_PARAMS");
  }

  if(failures == 0) {
    printf("LR2021 FLRC Match123/511: ALL CHECKS PASSED\n");
    return(0);
  }
  printf("LR2021 FLRC Match123/511: %d check(s) FAILED\n", failures);
  return(1);
}
