#include <sdkconfig.h>

#if !defined(CONFIG_BENCH_MODE_AUTONOMOUS_TX) && !defined(CONFIG_BENCH_MODE_AUTONOMOUS_RX) && !defined(CONFIG_BENCH_MODE_RANGE_TX) && !defined(CONFIG_BENCH_MODE_RANGE_RX) && !defined(CONFIG_BENCH_MODE_DUMP) && !defined(CONFIG_BENCH_MODE_SPI_LOOPBACK) && !defined(CONFIG_BENCH_MODE_CONTINUITY) && !defined(CONFIG_BENCH_MODE_FIFO_TEST) && !defined(CONFIG_BENCH_MODE_FIFO_TX) && !defined(CONFIG_BENCH_MODE_PROFILE) && !defined(CONFIG_BENCH_MODE_FAST_RX) && !defined(CONFIG_BENCH_MODE_FIPS_BRIDGE)

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include <RadioLib.h>
#include "EspHalC3.h"
#include "prbs.h"

static const char *TAG = "BENCH";

#define LR2021_SCK   6
#define LR2021_MISO  2
#define LR2021_MOSI  7
#define LR2021_NSS   10
#define LR2021_BUSY  4
#define LR2021_RST   3
#define LR2021_DIO9  5

static EspHalC3 *hal __attribute__((unused)) = nullptr;
static Module *mod __attribute__((unused)) = nullptr;
static LR2021 *radio __attribute__((unused)) = nullptr;

enum BenchMode { MODE_FLRC, MODE_LORA };
enum BenchRole { ROLE_NONE, ROLE_TX, ROLE_RX };

struct BenchConfig {
    BenchMode mode;
    float freq;
    uint16_t br;
    uint8_t sf;
    float bw;
    uint8_t cr;
    int8_t pwr;
    uint16_t pktSize;
    uint16_t pktCount;
    uint16_t txDelayMs;
    uint16_t preambleLen;
    BenchRole role;
};

static BenchConfig cfg = {
    MODE_FLRC, 868.0f, 325, 9, 125.0f,
    RADIOLIB_LR2021_FLRC_CR_3_4, 22, 50, 1000, 5, 16, ROLE_NONE
};

static volatile bool rxFlag = false;
static bool running = false;
static bool done = false;

static uint32_t txSent = 0;
static uint32_t txErrors = 0;
static uint32_t txStartTime = 0;
static uint32_t txElapsedMs = 0;
static uint32_t txCurrentPkt = 0;

static uint32_t rxReceived = 0;
static uint32_t rxCrcErrors = 0;
static uint32_t rxStartTime = 0;
static uint32_t rxElapsedMs = 0;
static uint32_t rxTotalSentByTx = 0;

static int32_t rssiSum = 0;
static uint32_t rssiCount = 0;
static int16_t rssiMin = 32767;
static int16_t rssiMax = -32768;
static float snrSum = 0;
static float snrMin = 999.0f;
static float snrMax = -999.0f;

static uint32_t rxPayloadCorrupt = 0;
static uint32_t rxBitErrorsTotal = 0;
static uint32_t rxBitsCheckedTotal = 0;
static uint32_t rxOutOfOrder = 0;
static uint32_t rxLastSeq = 0xFFFFFFFF;

static uint8_t *rxBitfield = nullptr;
static uint32_t rxBitfieldSize = 0;

static void IRAM_ATTR onRxIrq(void) {
    rxFlag = true;
}

static void setSeqBit(uint32_t seq) {
    if (seq < cfg.pktCount && rxBitfield) {
        rxBitfield[seq / 8] |= (1 << (seq % 8));
    }
}

static void resetCounters() {
    txSent = txErrors = txCurrentPkt = 0;
    rxReceived = rxCrcErrors = rxTotalSentByTx = 0;
    rssiSum = rssiCount = 0;
    rssiMin = 32767; rssiMax = -32768;
    snrSum = 0; snrMin = 999.0f; snrMax = -999.0f;
    rxPayloadCorrupt = rxBitErrorsTotal = rxBitsCheckedTotal = 0;
    rxOutOfOrder = 0; rxLastSeq = 0xFFFFFFFF;
    if (rxBitfield) { free(rxBitfield); rxBitfield = nullptr; }
    rxBitfieldSize = 0;
}

static int16_t initRadio() {
    int16_t state;
    if (cfg.mode == MODE_FLRC) {
        ESP_LOGI(TAG, "Init FLRC %.1f MHz BR=%d CR=0x%02X PWR=%d",
                 cfg.freq, cfg.br, cfg.cr, cfg.pwr);
        state = radio->beginFLRC(cfg.freq, cfg.br, cfg.cr, cfg.pwr,
                                  cfg.preambleLen, RADIOLIB_SHAPING_0_5, 0.0f);
    } else {
        ESP_LOGI(TAG, "Init LoRa %.1f MHz SF%d BW=%.0f CR=%d PWR=%d",
                 cfg.freq, cfg.sf, cfg.bw, cfg.cr, cfg.pwr);
        state = radio->begin(cfg.freq, cfg.bw, cfg.sf, cfg.cr,
                            RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE,
                            cfg.pwr, cfg.preambleLen, 0.0f);
    }
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "Radio init failed: %d", state);
        return state;
    }
    ESP_LOGI(TAG, "Radio init OK, BUSY=%d NSS=%d RST=%d IRQ=%d",
             gpio_get_level((gpio_num_t)LR2021_BUSY),
             gpio_get_level((gpio_num_t)LR2021_NSS),
             gpio_get_level((gpio_num_t)LR2021_RST),
             gpio_get_level((gpio_num_t)LR2021_DIO9));
    return state;
}

static void runTx() {
    resetCounters();
    int16_t state = initRadio();
    if (state != RADIOLIB_ERR_NONE) return;

    ESP_LOGI(TAG, "TX: Starting %d pkts, size=%d, delay=%dms in 2s...",
             cfg.pktCount, cfg.pktSize, cfg.txDelayMs);
    vTaskDelay(pdMS_TO_TICKS(2000));

    txStartTime = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint8_t buf[255];

    for (txCurrentPkt = 0; txCurrentPkt < cfg.pktCount; txCurrentPkt++) {
        buf[0] = (txCurrentPkt >> 24) & 0xFF;
        buf[1] = (txCurrentPkt >> 16) & 0xFF;
        buf[2] = (txCurrentPkt >> 8) & 0xFF;
        buf[3] = txCurrentPkt & 0xFF;
        if (cfg.pktSize > 4) {
            prbs15_fill(buf + 4, cfg.pktSize - 4, txCurrentPkt);
        }

        state = radio->transmit(buf, cfg.pktSize);
        if (state == RADIOLIB_ERR_NONE) {
            txSent++;
        } else {
            txErrors++;
            if (txErrors <= 10 || txErrors % 100 == 0) {
                ESP_LOGW(TAG, "TX err #%d at pkt %lu: %d (BUSY=%d)",
                         (int)txErrors, (unsigned long)txCurrentPkt, state,
                         gpio_get_level((gpio_num_t)LR2021_BUSY));
            }
        }

        if (txCurrentPkt == 0) {
            ESP_LOGI(TAG, "First pkt result: %d (BUSY=%d)", state,
                     gpio_get_level((gpio_num_t)LR2021_BUSY));
        }

        if (txCurrentPkt > 0 && txCurrentPkt % 100 == 0) {
            ESP_LOGI(TAG, "TX: %lu/%d", (unsigned long)txCurrentPkt, cfg.pktCount);
        }

        if (cfg.txDelayMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(cfg.txDelayMs));
        }
    }

    txElapsedMs = (uint32_t)(esp_timer_get_time() / 1000ULL) - txStartTime;

    uint8_t endBuf[8] = {0xDE, 0xAD, 0xBE, 0xEF,
                         (uint8_t)(txSent >> 24), (uint8_t)(txSent >> 16),
                         (uint8_t)(txSent >> 8), (uint8_t)(txSent)};
    radio->transmit(endBuf, 8);

    float sec = txElapsedMs / 1000.0f;
    float throughput = (txSent > 0 && sec > 0) ? (txSent * cfg.pktSize * 8.0f) / (sec * 1000.0f) : 0;
    ESP_LOGI(TAG, "=== TX RESULTS ===");
    ESP_LOGI(TAG, "sent,%lu", (unsigned long)txSent);
    ESP_LOGI(TAG, "tx_errors,%lu", (unsigned long)txErrors);
    ESP_LOGI(TAG, "elapsed_ms,%lu", (unsigned long)txElapsedMs);
    ESP_LOGI(TAG, "throughput_kbps,%.1f", throughput);
    ESP_LOGI(TAG, "time_per_pkt_ms,%.2f", txSent > 0 ? txElapsedMs / (float)txSent : 0);
    ESP_LOGI(TAG, "=== TX END ===");
}

static void runRx() {
    resetCounters();
    int16_t state = initRadio();
    if (state != RADIOLIB_ERR_NONE) return;

    rxBitfieldSize = (cfg.pktCount + 7) / 8;
    rxBitfield = (uint8_t *)calloc(rxBitfieldSize, 1);
    if (!rxBitfield) {
        ESP_LOGE(TAG, "Out of memory for bitfield");
        return;
    }

    radio->setPacketReceivedAction(onRxIrq);
    rxStartTime = (uint32_t)(esp_timer_get_time() / 1000ULL);
    radio->startReceive();
    running = true;
    done = false;

    ESP_LOGI(TAG, "RX: Waiting for packets...");

    uint32_t lastPrint = 0;
    while (running) {
        if (rxFlag) {
            rxFlag = false;
            int16_t len = radio->getPacketLength();
            if (len <= 0) {
                radio->standby();
                radio->startReceive();
                continue;
            }

            uint8_t buf[256];
            state = radio->readData(buf, len);

            if (len == 8 && buf[0] == 0xDE && buf[1] == 0xAD &&
                buf[2] == 0xBE && buf[3] == 0xEF) {
                rxTotalSentByTx = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                                  ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
                rxElapsedMs = (uint32_t)(esp_timer_get_time() / 1000ULL) - rxStartTime;
                running = false;
                done = true;
                radio->standby();
                continue;
            }

            if (state != RADIOLIB_ERR_NONE) {
                rxCrcErrors++;
                radio->standby();
                radio->startReceive();
                continue;
            }

            if (len < 4) {
                radio->standby();
                radio->startReceive();
                continue;
            }

            uint32_t seq = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];

            float rssi = radio->getRSSI(false);
            int16_t rssi_i = (int16_t)(rssi);
            float snr = 0;
            if (cfg.mode == MODE_LORA) snr = radio->getSNR();

            rssiSum += rssi_i; rssiCount++;
            if (rssi_i < rssiMin) rssiMin = rssi_i;
            if (rssi_i > rssiMax) rssiMax = rssi_i;
            snrSum += snr;
            if (snr < snrMin) snrMin = snr;
            if (snr > snrMax) snrMax = snr;

            setSeqBit(seq);

            if (rxLastSeq != 0xFFFFFFFF && seq < rxLastSeq) {
                rxOutOfOrder++;
            }
            rxLastSeq = seq;

            if (len > 4) {
                uint16_t bytesBad = 0;
                uint16_t bitErr = prbs15_verify(buf + 4, len - 4, seq, &bytesBad);
                if (bitErr > 0) {
                    rxPayloadCorrupt++;
                    rxBitErrorsTotal += bitErr;
                }
                rxBitsCheckedTotal += (len - 4) * 8;
            }

            rxReceived++;

            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (rxReceived % 100 == 0 && now - lastPrint > 500) {
                ESP_LOGI(TAG, "RX: %lu pkts, RSSI=%.0f, SNR=%.1f, seq=%lu",
                         (unsigned long)rxReceived, rssi, snr, (unsigned long)seq);
                lastPrint = now;
            }

            radio->standby();
            radio->startReceive();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (done) {
        float sec = rxElapsedMs / 1000.0f;
        uint32_t total = rxTotalSentByTx > 0 ? rxTotalSentByTx : rxReceived;
        uint32_t lost = total > rxReceived ? total - rxReceived : 0;
        float perPct = total > 0 ? (lost * 100.0f) / total : 0;
        float throughput = (sec > 0) ? (rxReceived * cfg.pktSize * 8.0f) / (sec * 1000.0f) : 0;
        float berPct = rxBitsCheckedTotal > 0 ? (rxBitErrorsTotal * 100.0f) / rxBitsCheckedTotal : 0;
        float avgRssi = rssiCount > 0 ? (float)rssiSum / rssiCount : 0;
        float avgSnr = rssiCount > 0 ? snrSum / rssiCount : 0;

        ESP_LOGI(TAG, "=== RX RESULTS ===");
        ESP_LOGI(TAG, "received,%lu", (unsigned long)rxReceived);
        ESP_LOGI(TAG, "crc_errors,%lu", (unsigned long)rxCrcErrors);
        ESP_LOGI(TAG, "lost,%lu", (unsigned long)lost);
        ESP_LOGI(TAG, "total_sent_by_tx,%lu", (unsigned long)rxTotalSentByTx);
        ESP_LOGI(TAG, "elapsed_ms,%lu", (unsigned long)rxElapsedMs);
        ESP_LOGI(TAG, "throughput_kbps,%.1f", throughput);
        ESP_LOGI(TAG, "per_pct,%.3f", perPct);
        ESP_LOGI(TAG, "ber_pct,%.6f", berPct);
        ESP_LOGI(TAG, "avg_rssi,%.1f", avgRssi);
        ESP_LOGI(TAG, "min_rssi,%d", rssiMin == 32767 ? 0 : rssiMin);
        ESP_LOGI(TAG, "max_rssi,%d", rssiMax == -32768 ? 0 : rssiMax);
        ESP_LOGI(TAG, "avg_snr,%.1f", avgSnr);
        ESP_LOGI(TAG, "payload_corrupt,%lu", (unsigned long)rxPayloadCorrupt);
        ESP_LOGI(TAG, "bit_errors_total,%lu", (unsigned long)rxBitErrorsTotal);
        ESP_LOGI(TAG, "bits_checked_total,%lu", (unsigned long)rxBitsCheckedTotal);
        ESP_LOGI(TAG, "out_of_order,%lu", (unsigned long)rxOutOfOrder);

        if (rxBitfield && rxTotalSentByTx > 0) {
            printf("SEQGAPS:");
            bool inGap = false;
            uint32_t gapStart = 0;
            for (uint32_t i = 0; i < rxTotalSentByTx && i < cfg.pktCount; i++) {
                bool recv = rxBitfield[i / 8] & (1 << (i % 8));
                if (!recv && !inGap) { gapStart = i; inGap = true; }
                else if (recv && inGap) {
                    printf(" %lu-%lu", (unsigned long)gapStart, (unsigned long)(i - 1));
                    inGap = false;
                }
            }
            if (inGap) printf(" %lu-%lu", (unsigned long)gapStart, (unsigned long)(cfg.pktCount - 1));
            printf("\n");
        }

        ESP_LOGI(TAG, "=== RX END ===");
    }

    if (rxBitfield) { free(rxBitfield); rxBitfield = nullptr; }
}

static void processCommand(const char *cmd) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (strlen(cmd) == 0) return;

    if (strcmp(cmd, "CONFIG") == 0) {
        ESP_LOGI(TAG, "mode=%s freq=%.1f br=%d sf=%d bw=%.0f cr=0x%02X pwr=%d size=%d count=%d delay=%d role=%s",
                 cfg.mode == MODE_FLRC ? "FLRC" : "LORA", cfg.freq, cfg.br, cfg.sf, cfg.bw,
                 cfg.cr, cfg.pwr, cfg.pktSize, cfg.pktCount, cfg.txDelayMs,
                 cfg.role == ROLE_TX ? "TX" : cfg.role == ROLE_RX ? "RX" : "NONE");
    } else if (strncmp(cmd, "MODE ", 5) == 0) {
        if (strcmp(cmd + 5, "FLRC") == 0) cfg.mode = MODE_FLRC;
        else if (strcmp(cmd + 5, "LORA") == 0) cfg.mode = MODE_LORA;
    } else if (strncmp(cmd, "FREQ ", 5) == 0) { cfg.freq = atof(cmd + 5); }
    else if (strncmp(cmd, "BR ", 3) == 0) { cfg.br = atoi(cmd + 3); }
    else if (strncmp(cmd, "SF ", 3) == 0) { cfg.sf = atoi(cmd + 3); }
    else if (strncmp(cmd, "BW ", 3) == 0) { cfg.bw = atof(cmd + 3); }
    else if (strncmp(cmd, "CR ", 3) == 0) { cfg.cr = (uint8_t)strtol(cmd + 3, nullptr, 0); }
    else if (strncmp(cmd, "PWR ", 4) == 0) { cfg.pwr = atoi(cmd + 4); }
    else if (strncmp(cmd, "SIZE ", 5) == 0) { cfg.pktSize = atoi(cmd + 5); }
    else if (strncmp(cmd, "COUNT ", 6) == 0) { cfg.pktCount = atoi(cmd + 6); }
    else if (strncmp(cmd, "DELAY ", 6) == 0) { cfg.txDelayMs = atoi(cmd + 6); }
    else if (strncmp(cmd, "PREAMBLE ", 9) == 0) { cfg.preambleLen = atoi(cmd + 9); }
    else if (strncmp(cmd, "ROLE ", 5) == 0) {
        if (strcmp(cmd + 5, "TX") == 0) cfg.role = ROLE_TX;
        else if (strcmp(cmd + 5, "RX") == 0) cfg.role = ROLE_RX;
    } else if (strcmp(cmd, "RUN") == 0) {
        if (cfg.role == ROLE_TX) runTx();
        else if (cfg.role == ROLE_RX) runRx();
        else ESP_LOGE(TAG, "Set ROLE first");
    } else if (strcmp(cmd, "HELP") == 0) {
        ESP_LOGI(TAG, "Commands: MODE FLRC|LORA, FREQ, BR, SF, BW, CR, PWR, SIZE, COUNT, DELAY, PREAMBLE, ROLE TX|RX, RUN, CONFIG, HELP");
    } else {
        ESP_LOGW(TAG, "Unknown: %s", cmd);
    }
}

#define CMD_BUF_SIZE 256
static char cmdBuf[CMD_BUF_SIZE];
static uint16_t cmdIdx = 0;

static void __attribute__((unused)) stdin_read_task(void *arg) {
    while (true) {
        int c = fgetc(stdin);
        if (c != EOF) {
            if (c == '\n' || c == '\r') {
                if (cmdIdx > 0) {
                    cmdBuf[cmdIdx] = '\0';
                    processCommand(cmdBuf);
                    cmdIdx = 0;
                }
            } else if (cmdIdx < CMD_BUF_SIZE - 1) {
                cmdBuf[cmdIdx++] = (char)c;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
 }

extern "C" void app_main() {
    ESP_LOGI(TAG, "=== LR2021 ESP-IDF Benchmarker v1.1 ===");
    ESP_LOGI(TAG, "Type HELP for commands");

    printf("  SPI clock = %d Hz\n", ESPHAL_C3_SPI_HZ);
    fflush(stdout);

    esp_task_wdt_deinit();

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    hal = new EspHalC3(LR2021_SCK, LR2021_MISO, LR2021_MOSI);
    hal->setCsPin(LR2021_NSS);
    hal->setBusyPin(LR2021_BUSY);
    mod = new Module(hal, LR2021_NSS, LR2021_DIO9, LR2021_RST, LR2021_BUSY);
    radio = new LR2021(mod);
    radio->irqDioNum = 9;

    xTaskCreate(stdin_read_task, "cmd", 4096, NULL, 5, NULL);
}

#endif
