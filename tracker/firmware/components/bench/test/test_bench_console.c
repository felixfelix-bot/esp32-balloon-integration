/**
 * @file    test_bench_console.c
 * @brief   Host parity + behaviour suite for the ESP32-C3 BENCH console.
 *
 * Compiled and run by tests/test_bench_host.py with the host gcc against the
 * vendored E80 console trio plus bench_console.c. Zero ESP-IDF/RadioLib
 * dependencies: bench_radio_ops_t is faked below.
 *
 * Oracles:
 *   - tests/golden/e80-flrc-session.txt  -> byte-exact PKT parity (25 col)
 *   - tests/golden/e80-lora-24col.txt    -> append-only 24-col tolerance
 *   - spec §4 PRBS-15 bytes, §5 CRC-16/CCITT-FALSE vectors
 *
 * Every check is counted; the harness prints "=== Results: P/T passed ==="
 * and exits non-zero on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bench_console.h"
#include "bench_cmd.h"
#include "bench_pkt.h"
#include "bench_stats.h"
#include "bench_payload.h"
#include "prbs.h"
#include "buffer.h"

#ifndef GOLDEN_DIR
#define GOLDEN_DIR "tests/golden"
#endif

/* ------------------------------------------------------------------ harness */

static int g_pass;
static int g_fail;

static void check(int cond, const char *what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && strcmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n  got : [%s]\n  want: [%s]\n", what,
               got ? got : "(null)", want);
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *what)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s (got %lu want %lu)\n", what, (unsigned long)got,
               (unsigned long)want);
    }
}

static FILE *open_golden(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", GOLDEN_DIR, name);
    return fopen(path, "r");
}

static void strip_nl(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/** Split a CSV line in place; returns the field count (capped at max). */
static int split_csv(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (*p != '\0') {
        if (*p == ',') {
            *p = '\0';
            if (n >= max) {
                return n;
            }
            fields[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/* ------------------------------------------------------------- fake radio */

#define TXLOG 4
typedef struct {
    uint32_t apply_calls;
    uint32_t rx_arm_calls;
    uint32_t tx_calls;
    uint32_t sleep_calls;
    uint32_t wake_calls;
    uint32_t delay_total_us;
    uint64_t now_us;
    uint8_t  txlog[TXLOG][BENCH_FLRC_MAX_PAYLOAD];
    uint16_t txlog_len[TXLOG];
} fake_radio_t;

static fake_radio_t g_radio;

static int fake_apply(void *ctx, const bench_console_t *st)
{
    fake_radio_t *r = (fake_radio_t *)ctx;
    r->apply_calls++;
    (void)st;
    return 0;
}

static int fake_rx_arm(void *ctx, const bench_console_t *st)
{
    fake_radio_t *r = (fake_radio_t *)ctx;
    r->rx_arm_calls++;
    (void)st;
    return 0;
}

static int fake_tx_packet(void *ctx, const uint8_t *buf, uint16_t len)
{
    fake_radio_t *r = (fake_radio_t *)ctx;
    if (r->tx_calls < TXLOG) {
        memcpy(r->txlog[r->tx_calls], buf, len);
        r->txlog_len[r->tx_calls] = len;
    }
    r->tx_calls++;
    return 0;
}

static void fake_sleep(void *ctx) { ((fake_radio_t *)ctx)->sleep_calls++; }
static void fake_wake(void *ctx) { ((fake_radio_t *)ctx)->wake_calls++; }

static void fake_delay_us(void *ctx, uint32_t us)
{
    fake_radio_t *r = (fake_radio_t *)ctx;
    r->delay_total_us += us;
    r->now_us += us;
}

static uint64_t fake_now_us(void *ctx) { return ((fake_radio_t *)ctx)->now_us; }

static int fake_chip_version(void *ctx, uint8_t *maj, uint8_t *min)
{
    (void)ctx;
    *maj = 2;
    *min = 1;
    return 0;
}

static const bench_radio_ops_t OPS = {
    .ctx = &g_radio,
    .apply = fake_apply,
    .rx_arm = fake_rx_arm,
    .tx_packet = fake_tx_packet,
    .sleep = fake_sleep,
    .wake = fake_wake,
    .delay_us = fake_delay_us,
    .now_us = fake_now_us,
    .chip_version = fake_chip_version,
};

/* ------------------------------------------------------------- emit sink */

#define EMIT_MAX 24
static char g_emit[EMIT_MAX][BENCH_LINE_MAX + 32];
static int g_emit_n;

static void emit_sink(const char *line)
{
    if (g_emit_n < EMIT_MAX) {
        snprintf(g_emit[g_emit_n], sizeof(g_emit[0]), "%s", line);
        g_emit_n++;
    }
}

static void emit_reset(void) { g_emit_n = 0; }

static bench_console_t g_st;

static void console_reset(void)
{
    memset(&g_radio, 0, sizeof(g_radio));
    emit_reset();
    bench_console_init(&g_st, &OPS, "test123", "cold");
    bench_console_set_emit(&g_st, emit_sink);
}

/* ------------------------------------------------------------- §4 / §5 math */

static void test_prbs15_golden(void)
{
    /* spec §4: seq 0 and seq 1 both produce DD D8 CC D2 AA EF FE 60 */
    static const uint8_t want[8] = { 0xDD, 0xD8, 0xCC, 0xD2, 0xAA, 0xEF, 0xFE, 0x60 };
    uint8_t buf[8];

    prbs15_fill(buf, sizeof(buf), 0);
    check(memcmp(buf, want, sizeof(want)) == 0, "prbs15_fill(seq=0) golden bytes");
    prbs15_fill(buf, sizeof(buf), 1);
    check(memcmp(buf, want, sizeof(want)) == 0, "prbs15_fill(seq=1) golden bytes");

    uint16_t bad = 0xFFFF;
    uint16_t bits = prbs15_verify(want, sizeof(want), 0, &bad);
    check_u32(bits, 0, "prbs15_verify clean stream bit errors");
    check_u32(bad, 0, "prbs15_verify clean stream bytes_bad");

    uint8_t corrupt[8];
    memcpy(corrupt, want, sizeof(want));
    corrupt[0] ^= 0x01;
    bits = prbs15_verify(corrupt, sizeof(corrupt), 0, &bad);
    check_u32(bits, 1, "prbs15_verify single-bit flip");
    check_u32(bad, 1, "prbs15_verify single-bit flip bytes_bad");
}

static void test_crc16_golden(void)
{
    /* spec §5 golden vectors */
    uint8_t z64[64];
    uint8_t seq4096[4096];
    memset(z64, 0, sizeof(z64));
    for (int i = 0; i < 4096; i++) {
        seq4096[i] = (uint8_t)(i % 256);
    }
    check_u32(crc16_ccitt_false((const uint8_t *)"123456789", 9), 0x29B1,
              "crc16 \"123456789\" -> 0x29B1");
    check_u32(crc16_ccitt_false(z64, sizeof(z64)), 0xD6DA,
              "crc16 64x0x00 -> 0xD6DA");
    check_u32(crc16_ccitt_false(seq4096, sizeof(seq4096)), 0x0F69,
              "crc16 4096x(i%256) -> 0x0F69");
}

static void test_payload_pcrc16_golden(void)
{
    /* spec §4: 32-byte payload (4-byte BE seq header + 28 PRBS bytes) */
    uint8_t buf[32];
    bench_payload_build(buf, sizeof(buf), 0);
    check_u32(buf[3], 0x00, "payload header seq=0 BE");
    check_u32(buf[0], 0x00, "payload header MSB");
    bench_payload_build(buf, sizeof(buf), 1);
    check_u32(buf[3], 0x01, "payload header seq=1 BE");
    check_u32(crc16_ccitt_false(buf, sizeof(buf)), 0x6998,
              "pcrc16 32B seq=1 -> 0x6998");
    bench_payload_build(buf, sizeof(buf), 0);
    check_u32(crc16_ccitt_false(buf, sizeof(buf)), 0x997E,
              "pcrc16 32B seq=0 -> 0x997E");
    uint16_t bad = 1;
    check_u32(bench_payload_verify(buf, sizeof(buf), 0, &bad), 0,
              "bench_payload_verify clean payload");
    check_u32(bad, 0, "bench_payload_verify clean payload bytes_bad");
}

/* ------------------------------------------------------------- PKT parity */

static void test_pkt_golden_flrc_25col(void)
{
    FILE *f = open_golden("e80-flrc-session.txt");
    check(f != NULL, "open tests/golden/e80-flrc-session.txt");
    if (f == NULL) {
        return;
    }

    char line[512];
    int rows = 0;
    int crc_fail_rows = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "PKT,", 4) != 0) {
            continue;
        }
        strip_nl(line);
        char work[512];
        snprintf(work, sizeof(work), "%s", line);
        char *fld[32];
        int nf = split_csv(work, fld, 32);
        check(nf == 25, "golden PKT row is 25 columns");

        bench_pkt_ctx_t ctx;
        ctx.session_id = (uint32_t)strtoul(fld[1], NULL, 10);
        ctx.config_id = (uint32_t)strtoul(fld[2], NULL, 10);
        ctx.replicate = (uint32_t)strtoul(fld[3], NULL, 10);

        bench_pkt_evt_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.seq = (uint32_t)strtoul(fld[4], NULL, 10);
        evt.ts_ms = (uint32_t)strtoul(fld[5], NULL, 10);
        evt.rssi_half_dbm = (int16_t)(strtol(fld[6], NULL, 10) * 2);
        evt.snr_qdb = (int8_t)(strtol(fld[7], NULL, 10) * 4);
        evt.mod = (strcmp(fld[12], "FLRC") == 0) ? BENCH_PKT_MOD_FLRC : BENCH_PKT_MOD_LORA;
        evt.sf = (uint8_t)strtoul(fld[13], NULL, 10);
        evt.bw_hz = (uint32_t)strtoul(fld[14], NULL, 10) * 1000u;
        evt.cr = (uint8_t)strtoul(fld[15], NULL, 10);
        evt.txpow_dbm = (int8_t)strtol(fld[16], NULL, 10);
        evt.len = (uint16_t)strtoul(fld[17], NULL, 10);
        evt.freq_hz = (uint32_t)strtoul(fld[11], NULL, 10);
        evt.bit_err = (uint16_t)strtoul(fld[9], NULL, 10);
        evt.bytes_bad = (uint16_t)strtoul(fld[10], NULL, 10);
        evt.pcrc16 = (uint16_t)strtoul(fld[24], NULL, 10);

        int crc_ok = atoi(fld[8]);
        char out[512];
        int n = bench_pkt_format(out, (int)sizeof(out), &ctx, &evt, crc_ok);

        check_str(out, line, "PKT 25-col golden parity");
        check_u32((uint32_t)n, (uint32_t)strlen(line), "bench_pkt_format length");
        if (crc_ok == 0) {
            crc_fail_rows++;
            check_u32((uint32_t)atoi(fld[4]), 0, "CRC-fail row pkt_idx==0");
            check_u32((uint32_t)atoi(fld[17]), 0, "CRC-fail row len==0");
            check_u32((uint32_t)atoi(fld[24]), 0, "CRC-fail row pcrc16==0");
        }
        rows++;
    }
    fclose(f);
    check(rows >= 3, "golden session contains >=3 PKT rows");
    check(crc_fail_rows >= 1, "golden session contains a CRC-fail row");
}

static void test_pkt_golden_lora_24col_append_only(void)
{
    FILE *f = open_golden("e80-lora-24col.txt");
    check(f != NULL, "open tests/golden/e80-lora-24col.txt");
    if (f == NULL) {
        return;
    }

    char line[512];
    int rows = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "PKT,", 4) != 0) {
            continue;
        }
        strip_nl(line);
        char work[512];
        snprintf(work, sizeof(work), "%s", line);
        char *fld[32];
        int nf = split_csv(work, fld, 32);
        check(nf == 24, "24-col golden row has 24 columns");

        bench_pkt_ctx_t ctx;
        ctx.session_id = (uint32_t)strtoul(fld[1], NULL, 10);
        ctx.config_id = (uint32_t)strtoul(fld[2], NULL, 10);
        ctx.replicate = (uint32_t)strtoul(fld[3], NULL, 10);

        bench_pkt_evt_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.seq = (uint32_t)strtoul(fld[4], NULL, 10);
        evt.ts_ms = (uint32_t)strtoul(fld[5], NULL, 10);
        evt.rssi_half_dbm = (int16_t)(strtol(fld[6], NULL, 10) * 2);
        evt.snr_qdb = (int8_t)(strtol(fld[7], NULL, 10) * 4);
        evt.mod = BENCH_PKT_MOD_LORA;
        evt.sf = (uint8_t)strtoul(fld[13], NULL, 10);
        evt.bw_hz = (uint32_t)strtoul(fld[14], NULL, 10) * 1000u;
        evt.cr = (uint8_t)strtoul(fld[15], NULL, 10);
        evt.txpow_dbm = (int8_t)strtol(fld[16], NULL, 10);
        evt.len = (uint16_t)strtoul(fld[17], NULL, 10);
        evt.freq_hz = (uint32_t)strtoul(fld[11], NULL, 10);
        evt.pcrc16 = 0;

        char out[512];
        bench_pkt_format(out, (int)sizeof(out), &ctx, &evt, atoi(fld[8]));

        /* Our line is 25 columns: fields 0..23 must be byte-identical to the
         * 24-column golden, field 24 is the appended pcrc16 (spec §3). */
        char work2[512];
        snprintf(work2, sizeof(work2), "%s", out);
        char *of[32];
        int nof = split_csv(work2, of, 32);
        check_u32((uint32_t)nof, 25, "our PKT line is 25 columns");
        int same = 1;
        for (int i = 0; i < 24 && i < nf && i < nof; i++) {
            if (strcmp(of[i], fld[i]) != 0) {
                same = 0;
                printf("  field %d: got [%s] want [%s]\n", i, of[i], fld[i]);
            }
        }
        check(same == 1, "24-col golden fields 0..23 preserved (append-only)");
        rows++;
    }
    fclose(f);
    check(rows >= 2, "24-col golden has >=2 PKT rows");
}

/* ---------------------------------------------------------- parser goldens */

static void test_cmd_parse_golden(void)
{
    bench_cmd_t c;

    check_u32(bench_cmd_parse("MOD flrc 650 5", &c), BENCH_CMD_OK, "parse MOD flrc ok");
    check_u32(c.mod, BENCH_MOD_FLRC, "MOD flrc sets mod");
    check_u32(c.br_bps, 650000, "MOD flrc 650 -> 650000 bps");
    check_u32((uint32_t)(int32_t)c.txpow_dbm, 5, "MOD flrc pa 5");

    check_u32(bench_cmd_parse("mod LoRa 8 125", &c), BENCH_CMD_OK, "MOD case-insensitive");
    check_u32(c.mod, BENCH_MOD_LORA, "MOD lora sets mod");
    check_u32(c.sf, 8, "MOD lora sf 8");
    check_u32(c.bw_hz, 125000, "MOD lora bw 125 kHz -> Hz");

    check_u32(bench_cmd_parse("START N=50 LEN=511 GAP=40000", &c), BENCH_CMD_OK, "parse START");
    check_u32(c.n_pkts, 50, "START N");
    check_u32(c.len_bytes, 511, "START LEN");
    check_u32(c.gap_us, 40000, "START GAP");
    check(c.has_len && c.has_n && c.has_gap, "START has_* flags");

    check_u32(bench_cmd_parse("START", &c), BENCH_CMD_OK, "bare START ok");
    check_u32(c.n_pkts, 100, "START default N=100");
    check_u32(c.len_bytes, 255, "START default LEN=255");
    check_u32(c.gap_us, 5000, "START default GAP=5000");

    check_u32(bench_cmd_parse("START LEN=512", &c), BENCH_CMD_E_RANGE, "START LEN=512 -> RANGE");
    check_u32(bench_cmd_parse("START LEN=5", &c), BENCH_CMD_E_RANGE, "START LEN=5 -> RANGE");
    check_u32(bench_cmd_parse("START N=0", &c), BENCH_CMD_E_RANGE, "START N=0 -> RANGE");
    check_u32(bench_cmd_parse("MOD lora 13 125", &c), BENCH_CMD_E_RANGE, "MOD lora sf13 -> RANGE");
    check_u32(bench_cmd_parse("MOD flrc 650 23", &c), BENCH_CMD_E_RANGE, "MOD flrc pa23 -> RANGE");
    check_u32(bench_cmd_parse("MOD flrc 123 5", &c), BENCH_CMD_E_RANGE, "MOD flrc bad br -> RANGE");
    check_u32(bench_cmd_parse("ID? x", &c), BENCH_CMD_E_SYNTAX, "ID? extra arg -> SYNTAX");
    check_u32(bench_cmd_parse("FOO", &c), BENCH_CMD_E_UNKNOWN, "unknown word -> UNKNOWN");
    check_u32(bench_cmd_parse("", &c), BENCH_CMD_E_SYNTAX, "empty line -> SYNTAX");

    check_str(bench_cmd_err_str(BENCH_CMD_OK), "OK", "err_str OK");
    check_str(bench_cmd_err_str(BENCH_CMD_E_SYNTAX), "SYNTAX", "err_str SYNTAX");
    check_str(bench_cmd_err_str(BENCH_CMD_E_ARG), "ARG", "err_str ARG");
    check_str(bench_cmd_err_str(BENCH_CMD_E_RANGE), "RANGE", "err_str RANGE");
    check_str(bench_cmd_err_str(BENCH_CMD_E_UNKNOWN), "UNKNOWN", "err_str UNKNOWN");
}

/* ------------------------------------------------------- console behaviour */

static void test_id_line(void)
{
    console_reset();
    check_str(bench_console_handle_line(&g_st, "ID?"),
              "ID ESP32BENCH v1.0 fw=test123 role=NONE armed=0 "
              "mod=flrc br=650000 freq=868000000 band=863-870MHz pa=10 "
              "pcap=+21dBm chip=2.1 radio=asleep boot=cold buf=0",
              "ID? reply (spec §2.1)");

    check_str(bench_console_handle_line(&g_st, "ROLE TX"),
              "OK ROLE TX (TX INHIBITED - SEND 'ARM TX' TO ENABLE)",
              "ROLE TX reply");
    check_str(bench_console_handle_line(&g_st, "ID?"),
              "ID ESP32BENCH v1.0 fw=test123 role=TX armed=0 "
              "mod=flrc br=650000 freq=868000000 band=863-870MHz pa=10 "
              "pcap=+21dBm chip=2.1 radio=awake boot=cold buf=0",
              "ID? after ROLE TX");

    check_str(bench_console_handle_line(&g_st, "ROLE RX"),
              "OK ROLE RX (CONTINUOUS)", "ROLE RX reply");
    check_str(bench_console_handle_line(&g_st, "ROLE NONE"),
              "OK ROLE NONE (RADIO ASLEEP)", "ROLE NONE reply");
    check(g_radio.sleep_calls > 0, "ROLE NONE sleeps the radio");
    check(strstr(bench_console_handle_line(&g_st, "HELP"), "CMDS:") != NULL,
          "HELP starts with CMDS:");
    check_str(bench_console_handle_line(&g_st, "BOGUS"), "ERR UNKNOWN",
              "unknown command -> ERR UNKNOWN");
    check_str(bench_console_handle_line(&g_st, "MOD"), "ERR SYNTAX",
              "MOD without args -> ERR SYNTAX");
    check_str(bench_console_handle_line(&g_st, "MOD lora 13 125"), "ERR RANGE",
              "bad MOD range -> ERR RANGE");
}

static void test_role_and_arm_gates(void)
{
    console_reset();

    /* ARM TX is only valid on a TX board (spec §2.3) */
    check_str(bench_console_handle_line(&g_st, "ARM TX"), "ERR ROLE NOT TX",
              "ARM TX with role NONE -> ERR ROLE NOT TX");
    bench_console_handle_line(&g_st, "ROLE RX");
    check_str(bench_console_handle_line(&g_st, "ARM TX"), "ERR ROLE NOT TX",
              "ARM TX on RX board -> ERR ROLE NOT TX");

    /* No command line can go from idle to radiating (two-step arm) */
    bench_console_handle_line(&g_st, "ROLE TX");
    check_str(bench_console_handle_line(&g_st, "START N=1 LEN=16"),
              "ERR NOT ARMED (SEND 'ARM TX')",
              "START before ARM -> ERR NOT ARMED");
    check_str(bench_console_handle_line(&g_st, "ARM TX"), "OK ARMED (TX ENABLED)",
              "ARM TX reply");
}

static void test_mod_freq_pa(void)
{
    console_reset();
    bench_console_handle_line(&g_st, "ROLE TX");

    check_str(bench_console_handle_line(&g_st, "MOD lora 8 125"),
              "OK MOD lora sf=8 bw=125000", "MOD lora reply");
    check_str(bench_console_handle_line(&g_st, "MOD flrc 650 5"),
              "OK MOD flrc br=650000 pa=5", "MOD flrc reply");

    check_str(bench_console_handle_line(&g_st, "FREQ 868000000"),
              "OK FREQ 868000000", "FREQ 868 reply");
    check_str(bench_console_handle_line(&g_st, "FREQ 2440000000"),
              "OK FREQ 2440000000", "FREQ 2440 reply (2.4 GHz window)");
    check_str(bench_console_handle_line(&g_st, "ID?"),
              "ID ESP32BENCH v1.0 fw=test123 role=TX armed=0 "
              "mod=flrc br=650000 freq=2440000000 band=2400-2480MHz pa=5 "
              "pcap=+21dBm chip=2.1 radio=awake boot=cold buf=0",
              "ID? band label follows the active window");
    check_str(bench_console_handle_line(&g_st, "FREQ 915000000"),
              "ERR BAND (ESP32BENCH: 863-870MHZ OR 2400-2480MHZ ONLY)",
              "FREQ out of band -> ERR BAND");

    /* indoor PA cap 10 dBm (spec §2.4/§2.6) */
    check_str(bench_console_handle_line(&g_st, "PA 12"),
              "ERR RANGE (INDOOR CAP 0-10 DBM; UNLOCK: POWER MODE OUTDOOR 2026)",
              "PA above indoor cap -> ERR RANGE");
    check_str(bench_console_handle_line(&g_st, "MOD flrc 650 12"),
              "ERR RANGE (INDOOR CAP 0-10 DBM; UNLOCK: POWER MODE OUTDOOR 2026)",
              "MOD flrc pa above indoor cap -> ERR RANGE");
    check_str(bench_console_handle_line(&g_st, "PA 5"), "OK PA 5 DBM", "PA 5 reply");

    /* outdoor unlock lifts to the board PA cap (spec §2.4, board pcap §2.1) */
    check_str(bench_console_handle_line(&g_st, "POWER MODE OUTDOOR 2026"),
              "OK POWER MODE OUTDOOR PIN 2026 ACCEPTED - TX POWER CAP LIFTED TO +21 DBM, "
              "OUTDOOR RANGE SESSIONS ONLY",
              "POWER MODE OUTDOOR unlock reply");
    check_str(bench_console_handle_line(&g_st, "PA 12"), "OK PA 12 DBM",
              "PA 12 after unlock");
    check_str(bench_console_handle_line(&g_st, "PA 22"), "ERR RANGE (0-21 DBM)",
              "PA above board cap -> ERR RANGE (0-21 DBM)");
    check_str(bench_console_handle_line(&g_st, "PA 5"), "OK PA 5 DBM",
              "PA back inside cap");
    check(g_radio.apply_calls >= 4, "MOD/FREQ/PA reprogram the radio");
}

static void test_start_len_caps(void)
{
    console_reset();
    bench_console_handle_line(&g_st, "ROLE TX");
    bench_console_handle_line(&g_st, "ARM TX");

    /* spec §6: parse cap 6..511 for both mods; runtime cap LoRa 255 / FLRC 511 */
    bench_console_handle_line(&g_st, "MOD flrc 650 5");
    check_str(bench_console_handle_line(&g_st, "START N=1 LEN=511 GAP=40000"),
              "OK START n=1 len=511 gap_us=40000 src=PRBS", "LEN 511 on FLRC -> OK");

    bench_console_handle_line(&g_st, "MOD lora 8 125");
    check_str(bench_console_handle_line(&g_st, "START N=1 LEN=300"),
              "ERR LEN (MAX 255 LORA / 511 FLRC)", "LEN 300 on LoRa -> ERR LEN");
    check_str(bench_console_handle_line(&g_st, "START N=1 LEN=255"),
              "OK START n=1 len=255 gap_us=5000 src=PRBS", "LEN 255 on LoRa -> OK");

    /* the parser refuses > 511 before the firmware cap is consulted */
    bench_console_handle_line(&g_st, "MOD flrc 650 5");
    check_str(bench_console_handle_line(&g_st, "START N=1 LEN=512"),
              "ERR RANGE", "LEN 512 -> parser ERR RANGE");
}

static void test_tx_burst_prbs(void)
{
    console_reset();
    bench_console_handle_line(&g_st, "ROLE TX");
    bench_console_handle_line(&g_st, "ARM TX");
    bench_console_handle_line(&g_st, "MOD flrc 650 5");
    bench_console_handle_line(&g_st, "SESSION 1234");
    emit_reset();
    g_radio.now_us = 0;

    check_str(bench_console_handle_line(&g_st, "START N=2 LEN=16 GAP=40000"),
              "OK START n=2 len=16 gap_us=40000 src=PRBS", "START TX reply");

    check_u32(g_radio.tx_calls, 2, "burst sends N packets");
    check_u32(g_radio.delay_total_us, 80000, "burst honours GAP");
    check(g_radio.sleep_calls == 1, "burst ends with the radio asleep");
    check(g_emit_n == 1, "burst emits one async line");
    if (g_emit_n >= 1) {
        check_str(g_emit[0], "TX DONE (RADIO ASLEEP)", "TX DONE async line");
    }

    /* payload = 4-byte BE seq header + PRBS15 fill (spec §4) */
    uint8_t want[16];
    bench_payload_build(want, sizeof(want), 0);
    check(memcmp(g_radio.txlog[0], want, sizeof(want)) == 0,
          "burst pkt 0 payload matches bench_payload_build(seq=0)");
    bench_payload_build(want, sizeof(want), 1);
    check(memcmp(g_radio.txlog[1], want, sizeof(want)) == 0,
          "burst pkt 1 payload matches bench_payload_build(seq=1)");
}

static void test_rx_events_and_stat(void)
{
    console_reset();
    bench_console_handle_line(&g_st, "SESSION 2608211756");
    bench_console_handle_line(&g_st, "CONFIG 0 1");
    check_u32(g_emit_n, 1, "CONFIG emits CONFIG_START");
    if (g_emit_n >= 1) {
        char want[BENCH_LINE_MAX];
        snprintf(want, sizeof(want), "CONFIG_START,0,1,%lu",
                 (unsigned long)(g_radio.now_us / 1000));
        check_str(g_emit[0], want, "CONFIG_START,<id>,<replicate>,<ts_ms>");
    }

    check_str(bench_console_handle_line(&g_st, "ROLE RX"),
              "OK ROLE RX (CONTINUOUS)", "RX role arms continuous RX");
    check(g_radio.rx_arm_calls >= 1, "ROLE RX arms the radio");

    check_str(bench_console_handle_line(&g_st, "START LEN=16"),
              "OK RX ARMED len=16", "START on the RX board arms the FIX_LEN window");

    uint8_t payload[16];
    bench_payload_build(payload, sizeof(payload), 0);
    emit_reset();
    g_radio.now_us = 3299000; /* ts_ms = 3299 */
    bench_console_rx_event(&g_st, -142, 0, 1, payload, sizeof(payload));

    check_u32(g_emit_n, 1, "RX_OK emits one PKT line");
    if (g_emit_n >= 1) {
        char want[BENCH_LINE_MAX];
        snprintf(want, sizeof(want),
                 "PKT,2608211756,0,1,0,3299,-71,0,1,0,0,868000000,FLRC,8,0,1,5,16,"
                 "0,0,0,0,0,0,%u",
                 (unsigned)crc16_ccitt_false(payload, sizeof(payload)));
        check_str(g_emit[0], want, "PKT line (25 col, pcrc16 over the payload)");
    }

    /* CRC-fail row: pkt_idx/len/pcrc16 are zeroed, RSSI/SNR still reported */
    emit_reset();
    g_radio.now_us = 3351000;
    bench_console_rx_event(&g_st, -142, 0, 0, payload, sizeof(payload));
    if (g_emit_n >= 1) {
        check_str(g_emit[0],
                  "PKT,2608211756,0,1,0,3351,-71,0,0,0,0,868000000,FLRC,8,0,1,5,0,"
                  "0,0,0,0,0,0,0",
                  "PKT CRC-fail row");
    } else {
        check(0, "CRC-fail emits a PKT line");
    }

    /* STAT accounting (spec §2.10) */
    g_radio.now_us = 4000000;
    {
        char want[BENCH_LINE_MAX];
        /* trials = rx_last_seq - rx_first_seq + 1 = 1; Wilson(1 success, 1 trial) */
        uint32_t lo = 0, hi = 0;
        bench_stats_wilson_ppm(1, 1, &lo, &hi);
        snprintf(want, sizeof(want),
                 "STAT role=RX sent=0 sent_ok=0 rx=1 crc_err=1 per_x1e6=0 "
                 "per_ci_x1e6=[%u,%u] elapsed_s=4.0 kbps=0 rssi_avg_dbm=-71.0 "
                 "rssi_min_dbm=-71.0 rssi_max_dbm=-71.0 snr_avg_db=0.0 cr=1 "
                 "session=2608211756 config=0 replicate=1 drops=0 gap_us=0 buf=0",
                 (unsigned)(1000000 - hi), (unsigned)(1000000 - lo));
        check_str(bench_console_handle_line(&g_st, "STAT?"), want,
                  "STAT? line (spec §2.10 field order)");
    }

    check_str(bench_console_handle_line(&g_st, "STOP"), "OK STOP (RADIO ASLEEP)",
              "STOP reply");
}

static void test_prbs_verify_on_rx(void)
{
    console_reset();
    bench_console_handle_line(&g_st, "ROLE RX");
    bench_console_handle_line(&g_st, "SESSION 42");

    uint8_t payload[16];
    bench_payload_build(payload, sizeof(payload), 7);

    /* PRBS OFF (default): bit_err/bytes_bad stay 0 (spec §2.11) */
    emit_reset();
    bench_console_rx_event(&g_st, -100, 0, 1, payload, sizeof(payload));
    if (g_emit_n >= 1) {
        char *fld[32];
        char work[BENCH_LINE_MAX];
        snprintf(work, sizeof(work), "%s", g_emit[0]);
        split_csv(work, fld, 32);
        check_u32((uint32_t)atoi(fld[9]), 0, "PRBS OFF -> bit_err 0");
        check_u32((uint32_t)atoi(fld[10]), 0, "PRBS OFF -> bytes_bad 0");
    }

    check_str(bench_console_handle_line(&g_st, "PRBS ON"), "OK PRBS ON",
              "PRBS ON reply");

    /* clean payload -> bit_err 0 */
    emit_reset();
    bench_console_rx_event(&g_st, -100, 0, 1, payload, sizeof(payload));
    if (g_emit_n >= 1) {
        char *fld[32];
        char work[BENCH_LINE_MAX];
        snprintf(work, sizeof(work), "%s", g_emit[0]);
        split_csv(work, fld, 32);
        check_u32((uint32_t)atoi(fld[9]), 0, "PRBS ON clean payload -> bit_err 0");
    }

    /* one flipped bit -> bit_err 1, bytes_bad 1 (spec §4 verification) */
    payload[5] ^= 0x01;
    emit_reset();
    bench_console_rx_event(&g_st, -100, 0, 1, payload, sizeof(payload));
    if (g_emit_n >= 1) {
        char *fld[32];
        char work[BENCH_LINE_MAX];
        snprintf(work, sizeof(work), "%s", g_emit[0]);
        split_csv(work, fld, 32);
        check_u32((uint32_t)atoi(fld[4]), 7, "PRBS pkts carry the header seq");
        check_u32((uint32_t)atoi(fld[9]), 1, "one flipped bit -> bit_err 1");
        check_u32((uint32_t)atoi(fld[10]), 1, "one flipped bit -> bytes_bad 1");
    }

    check_str(bench_console_handle_line(&g_st, "PRBS OFF"), "OK PRBS OFF",
              "PRBS OFF reply");
    check_str(bench_console_handle_line(&g_st, "QUIET ON"), "OK QUIET ON",
              "QUIET ON reply");
    emit_reset();
    bench_console_rx_event(&g_st, -100, 0, 1, payload, sizeof(payload));
    check_u32(g_emit_n, 0, "QUIET ON suppresses PKT lines");
    check_str(bench_console_handle_line(&g_st, "QUIET OFF"), "OK QUIET OFF",
              "QUIET OFF reply");
}

static void test_unsupported_extensions(void)
{
    console_reset();
    /* E80 vendor extensions (spec §2.13): porters MAY omit them and hosts MUST
     * NOT require them — every one of them answers ERR, never silence. */
    const char *lines[] = {
        "FLASH",
        "BUF LOAD 16 ABCD",
        "BUF CLEAR",
        "BUF STATUS",
        "BAND OVERRIDE 2026",
        "PRBS9 ON",
        "POWER MODE OUTDOOR 7", /* wrong pin: the unlock key is board-local */
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const char *reply = bench_console_handle_line(&g_st, lines[i]);
        check(strncmp(reply, "ERR ", 4) == 0, lines[i]);
    }
}

int main(void)
{
    printf("== PRBS-15 (spec 4) ==\n");
    test_prbs15_golden();
    printf("== CRC-16/CCITT-FALSE (spec 5) ==\n");
    test_crc16_golden();
    printf("== payload + pcrc16 goldens ==\n");
    test_payload_pcrc16_golden();
    printf("== PKT 25-col golden parity ==\n");
    test_pkt_golden_flrc_25col();
    printf("== PKT 24-col append-only tolerance ==\n");
    test_pkt_golden_lora_24col_append_only();
    printf("== parser goldens ==\n");
    test_cmd_parse_golden();
    printf("== console: ID/HELP/errors ==\n");
    test_id_line();
    printf("== console: role + arm gates ==\n");
    test_role_and_arm_gates();
    printf("== console: MOD/FREQ/PA ==\n");
    test_mod_freq_pa();
    printf("== console: LEN caps ==\n");
    test_start_len_caps();
    printf("== console: TX burst ==\n");
    test_tx_burst_prbs();
    printf("== console: RX events + STAT ==\n");
    test_rx_events_and_stat();
    printf("== console: PRBS verify ==\n");
    test_prbs_verify_on_rx();
    printf("== console: unsupported E80 extensions ==\n");
    test_unsupported_extensions();

    int total = g_pass + g_fail;
    printf("\n=== Results: %d/%d passed ===\n", g_pass, total);
    return g_fail == 0 ? 0 : 1;
}
