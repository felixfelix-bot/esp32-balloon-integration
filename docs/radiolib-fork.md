# RadioLib Fork — LR2021 FLRC Match123 + 511-byte payloads

## Why a fork

The range-test-proven FLRC configuration (RP2040 `multi_radio_sweep_rx_v4`,
see `~/repos/balloon-range-tests`) needs three things stock RadioLib cannot do:

1. **Sync-word match selection** — the receiver must match sync words 1+2+3
   (`sw_match=111`, "Match123") instead of stock Match1-only. Stock RadioLib
   hardcodes `syncMatch=0x01` in every `SET_FLRC_PACKET_PARAMS` emission.
2. **511-byte payloads** — FLRC's payload-length field is 16-bit, but
   RadioLib's `fixedPacketLengthMode()`/`setPacketMode()` path is `uint8_t`
   and caps everything at `RADIOLIB_LR2021_MAX_PACKET_LENGTH` (255).
3. **CALIB_FRONT_END before Rx** — mandatory per the LR2021 datasheet and
   required for the working 2.4G rx path (prior art calls it in every RX
   phase). Stock RadioLib never invokes it.

## Where things live

| Artifact | Location |
|---|---|
| Fork | https://github.com/felixfelix-bot/RadioLib |
| Branch | `lr2021-flrc-511-match123` (base: upstream `f403b9c3d`, post-7.6.0 master) |
| Patch file | `patches/radiolib-lr2021-flrc511-match123.patch` (`git am`-able, mirrors the fork commit) |
| Submodule | `tracker/firmware/components/RadioLib` → pinned to fork branch commit |
| `.gitmodules` | URL points at the fork (NOT `jgromes/RadioLib`) |

## API added

```cpp
// constants in LR2021_commands.h
RADIOLIB_LR2021_FLRC_SYNC_MATCH_1        // 0x01, stock default
RADIOLIB_LR2021_FLRC_SYNC_MATCH_1_2      // 0x03
RADIOLIB_LR2021_FLRC_SYNC_MATCH_1_2_3    // 0x07  <-- Match123

// new public setter (FLRC mode only)
radio.setFlrcSyncWordMatch(RADIOLIB_LR2021_FLRC_SYNC_MATCH_1_2_3);

// widened to uint16_t
radio.fixedPacketLengthMode(511);
```

Behavioral notes:

- `flrcSyncMatch` (default `0x01`) and `flrcPayloadLen` (default 255) are
  cached members, so re-configuration methods (`setPreambleLength`,
  `setCRC`, `setSyncWord`) preserve both — stock RadioLib used to reset the
  payload length to 255 on every re-emit.
- Packet-length caps are FLRC-conditional: FLRC allows 511, all other
  modems stay capped at 255 (enforced in `transmit()`, `stageMode()` TX and
  `setPacketMode()`).
- Rx staging (`stageMode()` RX case) now runs `CALIB_FRONT_END` right after
  `SET_RX_PATH`, with `freq/4` and the HF-path bit (`0x8000`) when above
  1500 MHz — byte-for-byte the prior-art algorithm.

## Golden SPI frames (host-verified)

After `beginFLRC()` + `setCRC(0)` + `setFlrcSyncWordMatch(MATCH_1_2_3)` +
`fixedPacketLengthMode(511)`:

```
SET_FLRC_PACKET_PARAMS (0x0249): 0x0E 0x7C 0x01 0xFF
  byte0 0x0E = agc preamble 16 bits (3) | sync word len 32 bits (2)
  byte1 0x7C = swTx 01 | sw_match 111 (Match123) | fixed 1 | crc 00
  byte2..3   = payload length 0x01FF (511)
```

Stock RadioLib emits `0x4C` here (`sw_match=001`); the RP2040 v4 range-test
firmware emits `0x7C`. SET_RX_PATH (`0x0201`) carries path byte `0x01` (HF),
matching prior art (RadioLib's default boost value `0x04` is a deliberate
divergence, accepted).

Test: `python3 -m pytest tests/test_c_host.py -k flrc` —
`TestLR2021FLRC::test_lr2021_flrc_match123_host` (TDD: RED commit `5d88e35`
preceded the implementation).

## Update policy

- The fork branch must always sit on top of an upstream `jgromes/RadioLib`
  commit — never diverge needlessly. Rebase when bumping the submodule.
- Keep `patches/radiolib-lr2021-flrc511-match123.patch` in sync with the
  fork commit: `git -C tracker/firmware/components/RadioLib format-patch
  <base>..HEAD --stdout > patches/radiolib-lr2021-flrc511-match123.patch`
- Upstreaming: the Match123 setter + 511-byte FLRC payloads are clean
  upstream candidates; CALIB_FRONT_END-before-Rx may need upstream discussion
  (it adds a command per `startReceive`). See task t_5dc7f346 for context.
