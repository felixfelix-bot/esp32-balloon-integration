/**
 * @file    bench_main.cpp
 * @brief   ESP32-C3 BENCH console glue (HARM-T4): RadioLib LR2021 + USB-CDC.
 *
 * The board-independent console logic (command dispatch, reply strings, LEN
 * caps, PKT formatting, STAT accounting) lives in `components/bench`
 * (spec BENCH-CONSOLE-SPEC v1.0) and is host-tested. This file is only the
 * hardware side:
 *
 *   - EspHalC3 + LR2021 bring-up on the C3 supermini wiring
 *     (SCK 6 / MISO 2 / MOSI 7 / NSS 10 / BUSY 4 / RST 3 / DIO9 5)
 *   - the golden cross-board FLRC RF configuration of spec §8
 *     (Match123, 32-bit sync word 0x12AD101B, chip CRC on, CR 3/4,
 *     preamble 32 bits, BT 1.0) applied before EVERY transmit and RX arm
 *   - a blocking TX burst loop (PRBS payloads) and an RX polling task that
 *     feeds received packets into the console
 *   - the two FreeRTOS tasks and the mutex that serialise console + radio
 *
 * Active only with CONFIG_BENCH_CONSOLE=y (see main/Kconfig.projbuild and
 * docs/bench-console-esp32.md for the build recipe). No tracker task starts
 * when it is on: app_main() hands the board to bench_console_task_start().
 */

#include <sdkconfig.h>

#if CONFIG_BENCH_CONSOLE

/* The FLRC packet-status getter (rssi_avg / rssi_sync) is declared private in
 * the RadioLib LR2021 module, and the public getRSSI(packet=true) path returns
 * 0 for FLRC. RADIOLIB_GODMODE (RadioLib's own access-relaxation switch) opens
 * the getter up so FLRC rows carry chip packet RSSI instead of the
 * instantaneous RSSI, which reads ~30 dB low on short FLRC payloads because the
 * 32-bit preamble cannot settle the AGC (see balloon-e80bench
 * src/radio_bench.c, radio_bench_rx_arm() comment). getFlrcPacketStatus() only
 * differs in *access*, never in class layout or vtable, so this TU-local
 * define is layout-compatible with the other TUs that compile RadioLib.
 * Follow-up (HARM-T7 / upstream): make getFlrcPacketStatus() public upstream
 * and drop this define. */

#define RADIOLIB_GODMODE (1)

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include <RadioLib.h>

#include "EspHalC3.h"
#include "bench_main.h"

extern "C" {
#include "bench_console.h"
#include "prbs.h"
}

/* --------------------------------------------------------------- hardware */

#define LR2021_SCK  6
#define LR2021_MISO 2
#define LR2021_MOSI 7
#define LR2021_NSS  10
#define LR2021_BUSY 4
#define LR2021_RST  3
#define LR2021_DIO9 5

#define BENCH_FLRC_PREAMBLE_BITS    32   /* spec §8 */
#define BENCH_LORA_PREAMBLE_SYMBOLS 8
#define BENCH_LORA_SYNC_WORD        0x12 /* private-network LoRa sync word */
#define BENCH_FLRC_SYNC_WORD_32     0x12AD101Bu /* spec §8 */
#define BENCH_CROSS_BOARD_CRC_BYTES 2    /* spec §8: chip CRC on, 2 bytes */
#define BENCH_TASK_STACK            4096
#define BENCH_LOCK_TIMEOUT_MS       10000

static EspHalC3 *s_hal = nullptr;
static Module *s_mod = nullptr;
static LR2021 *s_radio = nullptr;

static bench_console_t s_con;
static bench_radio_ops_t s_ops;
static SemaphoreHandle_t s_lock = nullptr;
static volatile bool s_rx_pending = false;

/* Radio bring-up cache: a modulation or frequency change needs a full
 * re-init (the LR2021 modem does not reconfigure reliably from register
 * writes alone — same reset-then-recalibrate path the E80 reference uses). */
static bool s_ready = false;
static bench_mod_t s_active_mod = BENCH_MOD_FLRC;
static uint32_t s_active_freq_hz = 0;
static uint16_t s_active_br_kbps = 0;

static void IRAM_ATTR on_rx_irq(void)
{
    s_rx_pending = true;
}

/* ------------------------------------------------------------------- ops */

static bool flrc_window(const bench_console_t *st, uint16_t *out)
{
    uint16_t len = (uint16_t)st->tx_len;
    if (len == 0) {
        len = 255; /* unset window: the FLRC default in E80's pkt params */
    }
    if (len > BENCH_FLRC_MAX_PAYLOAD) {
        return false;
    }
    *out = len;
    return true;
}

/** spec §8 golden cross-board FLRC configuration.
 *
 * Applied on EVERY arm, not just on config change: RadioLib's transmit() path
 * re-emits the FLRC packet parameters with the sync-match field hard-coded to
 * Match1 (LR2021.cpp, `setFlrcPacketParams(..., 0x01, ...)`), so a board that
 * transmits and later returns to RX would silently fall back to Match1-only —
 * the #1 cross-board FLRC breakage mode (harmonization plan §5). Re-applying
 * the golden parameters after any burst restores Match123. */
static void apply_flrc_golden(const bench_console_t *st)
{
    uint8_t sync[4] = {(uint8_t)(BENCH_FLRC_SYNC_WORD_32 >> 24),
                       (uint8_t)(BENCH_FLRC_SYNC_WORD_32 >> 16),
                       (uint8_t)(BENCH_FLRC_SYNC_WORD_32 >> 8),
                       (uint8_t)(BENCH_FLRC_SYNC_WORD_32)};
    uint16_t window = 255;

    (void)s_radio->setSyncWord(sync, sizeof(sync));       /* sync word #1 */
    (void)s_radio->setFlrcSyncWordMatch(RADIOLIB_LR2021_FLRC_SYNC_MATCH_1_2_3);
    (void)s_radio->setCRC(BENCH_CROSS_BOARD_CRC_BYTES);
    if (flrc_window(st, &window)) {
        (void)s_radio->fixedPacketLengthMode(window);     /* FIX_LEN window */
    }
}

static int radio_apply(void *ctx, const bench_console_t *st)
{
    (void)ctx;
    if (s_radio == nullptr) {
        return -1;
    }

    float freq_mhz = (float)st->freq_hz / 1000000.0f;
    int16_t rc = RADIOLIB_ERR_NONE;
    bool band_change = (st->mod != s_active_mod) || (st->freq_hz != s_active_freq_hz);

    if (!s_ready || band_change) {
        if (st->mod == BENCH_MOD_FLRC) {
            uint16_t br_kbps = (uint16_t)(st->br_bps / 1000u);
            rc = s_radio->beginFLRC(freq_mhz, br_kbps, RADIOLIB_LR2021_FLRC_CR_3_4,
                                    st->txpow_dbm, BENCH_FLRC_PREAMBLE_BITS,
                                    RADIOLIB_SHAPING_1_0, 0.0f);
            if (rc == RADIOLIB_ERR_NONE) {
                s_active_br_kbps = br_kbps;
            }
        } else {
            rc = s_radio->begin(freq_mhz, (float)st->bw_hz / 1000.0f, st->sf,
                                RADIOLIB_LR2021_LORA_CR_4_5, BENCH_LORA_SYNC_WORD,
                                st->txpow_dbm, BENCH_LORA_PREAMBLE_SYMBOLS, 0.0f);
        }
        if (rc != RADIOLIB_ERR_NONE) {
            s_ready = false;
            printf("ERR RADIO INIT (%d)\n", (int)rc);
            fflush(stdout);
            return (int)rc;
        }
        s_ready = true;
        s_active_mod = st->mod;
        s_active_freq_hz = st->freq_hz;
        s_active_br_kbps = (uint16_t)(st->br_bps / 1000u);
    } else {
        rc = s_radio->setFrequency(freq_mhz);
        if (rc == RADIOLIB_ERR_NONE) {
            rc = s_radio->setOutputPower(st->txpow_dbm);
        }
        if (rc != RADIOLIB_ERR_NONE) {
            s_ready = false;
            return (int)rc;
        }
    }

    if (st->mod == BENCH_MOD_FLRC) {
        apply_flrc_golden(st);
    } else {
        (void)s_radio->setCRC(1);                 /* LoRa CRC on */
        (void)s_radio->variablePacketLengthMode(BENCH_LORA_MAX_PAYLOAD);
    }
    return 0;
}

static int radio_rx_arm(void *ctx, const bench_console_t *st)
{
    (void)ctx;
    (void)st;
    if (s_radio == nullptr) {
        return -1;
    }
    s_rx_pending = false;
    int16_t rc = s_radio->startReceive();
    return (rc == RADIOLIB_ERR_NONE) ? 0 : (int)rc;
}

static int radio_tx_packet(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    if (s_radio == nullptr) {
        return -1;
    }
    /* Note: transmit() re-emits the FLRC packet parameters with match=0x01
     * (RadioLib residual, see apply_flrc_golden above). The console re-arms
     * through radio_apply() before every burst and RX arm, so Match123 is
     * restored before the next receive. */
    int16_t rc = s_radio->transmit(buf, (size_t)len);
    return (rc == RADIOLIB_ERR_NONE) ? 0 : (int)rc;
}

static void radio_sleep(void *ctx)
{
    (void)ctx;
    if (s_radio == nullptr) {
        return;
    }
    s_radio->sleep();
    /* A slept LR2021 is re-initialised from scratch on the next apply():
     * cheapest way to guarantee the golden config after a wake. */
    s_ready = false;
}

static void radio_wake(void *ctx)
{
    (void)ctx;
    if (s_radio == nullptr || !s_ready) {
        return; /* apply() will run the full bring-up */
    }
    (void)s_radio->standby();
}

static void radio_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us == 0) {
        return;
    }
    if (us >= 20000) {
        /* Long sweep gaps (spec §7: >= 40 ms for LEN > 256) yield the CPU so
         * the RX/USB tasks keep running and the radio IRQ stays serviced. */
        vTaskDelay(pdMS_TO_TICKS((us + 999U) / 1000U));
    } else {
        esp_rom_delay_us(us);
    }
}

static uint64_t radio_now_us(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time();
}

static int radio_chip_version(void *ctx, uint8_t *major, uint8_t *minor)
{
    (void)ctx;
    /* RadioLib's LR2021::getVersion() is private even with RADIOLIB_GODMODE
     * (it sits outside the access-guarded block) and no public wrapper exists;
     * every board in the registry reports chip=2.1 for the LR2021 revision
     * used by the NiceRF LR2021F33 modules (spec §2.1 crossboard-ids golden).
     * Verify on the target in HARM-T7. */
    *major = 2;
    *minor = 1;
    return 0;
}

/* ------------------------------------------------------------------- RX */

static void bench_poll_rx(void)
{
    if (!s_rx_pending) {
        return;
    }
    s_rx_pending = false;

    size_t len = s_radio->getPacketLength();
    uint8_t buf[BENCH_FLRC_MAX_PAYLOAD];
    if (len == 0 || len > sizeof(buf)) {
        (void)s_radio->startReceive(); /* spurious IRQ: re-arm and move on */
        return;
    }

    /* Packet status first (readData() clears the IRQ/FIFO state). */
    float rssi = 0.0f;
    int8_t snr_qdb = 0;
    if (s_con.mod == BENCH_MOD_FLRC) {
        uint16_t pkt_len = 0;
        float rssi_avg = 0.0f;
        float rssi_sync = 0.0f;
        uint8_t sync_word = 0;
        if (s_radio->getFlrcPacketStatus(&pkt_len, &rssi_avg, &rssi_sync,
                                         &sync_word) == RADIOLIB_ERR_NONE) {
            rssi = rssi_avg;
        }
    }

    int16_t rc = s_radio->readData(buf, len);
    int crc_ok = (rc == RADIOLIB_ERR_NONE);

    if (s_con.mod == BENCH_MOD_LORA) {
        rssi = s_radio->getRSSI(true);
        snr_qdb = (int8_t)lroundf(s_radio->getSNR() * 4.0f);
    }
    int16_t rssi_half = (int16_t)lroundf(rssi * 2.0f);

    bench_console_rx_event(&s_con, rssi_half, snr_qdb, crc_ok, buf,
                           (uint16_t)(crc_ok ? len : 0));

    (void)s_radio->standby();
    (void)s_radio->startReceive();
}

/* --------------------------------------------------------------- console */

static void bench_emit(const char *line)
{
    printf("%s\n", line);
    fflush(stdout);
}

static void bench_console_task(void *arg)
{
    (void)arg;
    char line[BENCH_CMD_LINE_MAX + 1];
    size_t n = 0;

    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n == 0) {
                continue;
            }
            line[n] = '\0';
            n = 0;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BENCH_LOCK_TIMEOUT_MS)) == pdTRUE) {
                const char *reply = bench_console_handle_line(&s_con, line);
                printf("%s\n", reply);
                fflush(stdout);
                xSemaphoreGive(s_lock);
            }
        } else if (n < BENCH_CMD_LINE_MAX) {
            line[n++] = (char)c;
        }
        /* longer lines are truncated (spec §1: hosts keep lines <= 96 chars) */
    }
}

static void bench_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BENCH_LOCK_TIMEOUT_MS)) == pdTRUE) {
            if (s_con.role == BENCH_ROLE_RX) {
                bench_poll_rx();
            } else {
                s_rx_pending = false;
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ------------------------------------------------------------- bootstrap */

extern "C" void bench_console_task_start(void)
{
    esp_task_wdt_deinit(); /* long blocking bursts / blocking serial reads */

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    s_hal = new EspHalC3(LR2021_SCK, LR2021_MISO, LR2021_MOSI);
    s_hal->setCsPin(LR2021_NSS);
    s_hal->setBusyPin(LR2021_BUSY);
    s_mod = new Module(s_hal, LR2021_NSS, LR2021_DIO9, LR2021_RST, LR2021_BUSY);
    s_radio = new LR2021(s_mod);
    s_radio->irqDioNum = LR2021_DIO9;
    s_radio->setPacketReceivedAction(on_rx_irq);

    s_ops.ctx = nullptr;
    s_ops.apply = radio_apply;
    s_ops.rx_arm = radio_rx_arm;
    s_ops.tx_packet = radio_tx_packet;
    s_ops.sleep = radio_sleep;
    s_ops.wake = radio_wake;
    s_ops.delay_us = radio_delay_us;
    s_ops.now_us = radio_now_us;
    s_ops.chip_version = radio_chip_version;

    bench_console_init(&s_con, &s_ops, BENCH_FW_SHA, "cold");
    bench_console_set_emit(&s_con, bench_emit);

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        printf("ERR BENCH MUTEX\n");
        return;
    }

    printf("\n%s\n", bench_console_banner(&s_con));
    fflush(stdout);

    xTaskCreate(bench_console_task, "bench-console", BENCH_TASK_STACK, NULL, 5, NULL);
    xTaskCreate(bench_rx_task, "bench-rx", BENCH_TASK_STACK, NULL, 6, NULL);

    /* The bench console owns the board: park app_main() forever. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif /* CONFIG_BENCH_CONSOLE */
