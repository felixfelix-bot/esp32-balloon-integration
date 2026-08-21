// MockLR2021Hal - deterministic SPI-capture HAL for LR2021 host tests.
//
// Records every CS-framed SPI transaction (MOSI bytes, in order) and emulates
// the LRxxxx two-transaction read protocol:
//   phase A: real 16-bit command is clocked out            -> out[0..1] != 0x0000
//   phase B: NOP read-back with 0-bit command width        -> out[0..1] == 0x0000
// Responses are canned per 16-bit command; every other byte is a "command OK"
// status (STAT_1 = 0x04 == CMD_OK<<1, see RADIOLIB_LR11X0_STAT_1_CMD_OK).
// Payload bytes land at buffIn[cmdLen(0) + status(2)] in phase B, matching
// Module::SPItransferStream's dataIn copy.
#ifndef MOCK_LR2021_HAL_H
#define MOCK_LR2021_HAL_H

#include <map>
#include <vector>
#include <cstdint>

#include "RadioLib.h"

class MockLR2021Hal : public RadioLibHal {
  public:
    MockLR2021Hal()
      : RadioLibHal(1 /* input */, 2 /* output */, 0 /* low */, 1 /* high */, 3 /* rising */, 4 /* falling */) {
    }

    // every SPI transaction, MOSI bytes, in chronological order
    std::vector<std::vector<uint8_t>> frames;

    // canned read payloads keyed by 16-bit command (LR2021 command codes)
    std::map<uint16_t, std::vector<uint8_t>> canned;

    void spiBegin() override {}
    void spiEnd() override {}
    void spiBeginTransaction() override {}
    void spiEndTransaction() override {}

    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override {
      frames.emplace_back(out, out + len);

      const bool nopPhase = (len >= 2) && (out[0] == 0x00) && (out[1] == 0x00);

      for(size_t i = 0; i < len; i++) {
        in[i] = STAT_OK;
      }

      if(nopPhase) {
        // read-back phase: deliver canned payload for the pending read command
        if(pendingRead != 0x0000) {
          auto it = canned.find(pendingRead);
          if(it != canned.end()) {
            for(size_t i = 0; (i < it->second.size()) && ((2 + i) < len); i++) {
              in[2 + i] = it->second[i];
            }
          }
          pendingRead = 0x0000;
        }
      } else if(len >= 2) {
        uint16_t cmd = ((uint16_t)out[0] << 8) | (uint16_t)out[1];
        if(canned.count(cmd) > 0) {
          pendingRead = cmd;
        }
      }
    }

    void pinMode(uint32_t, uint32_t) override {}
    void digitalWrite(uint32_t, uint32_t) override {}
    uint32_t digitalRead(uint32_t) override { return(0); }  // BUSY line never asserted
    void attachInterrupt(uint32_t, void (*)(void), uint32_t) override {}
    void detachInterrupt(uint32_t) override {}
    void delay(RadioLibTime_t) override {}
    void delayMicroseconds(RadioLibTime_t) override {}
    RadioLibTime_t millis() override { return(++this->now); }
    RadioLibTime_t micros() override { return((++this->now) * 1000); }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return(0); }

    // helpers for tests //

    // all transactions whose first two bytes equal `cmd`
    std::vector<std::vector<uint8_t>> findCmd(uint16_t cmd) const {
      std::vector<std::vector<uint8_t>> out;
      for(auto& f : this->frames) {
        if((f.size() >= 2) && (((uint16_t)f[0] << 8 | f[1]) == cmd)) {
          out.push_back(f);
        }
      }
      return(out);
    }

    static std::string hex(const std::vector<uint8_t>& v) {
      char buf[8];
      std::string s;
      for(uint8_t b : v) {
        snprintf(buf, sizeof(buf), "%02X ", b);
        s += buf;
      }
      return(s);
    }

  private:
    static constexpr uint8_t STAT_OK = 0x04;  // RADIOLIB_LR11X0_STAT_1_CMD_OK
    uint16_t pendingRead = 0x0000;
    RadioLibTime_t now = 0;
};

#endif
