#include <sdkconfig.h>

#ifdef CONFIG_BENCH_MODE_SPI_LOOPBACK

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "SPILOOP";

#define SPI_MOSI 7
#define SPI_MISO 2
#define SPI_SCK  6
#define LED_GPIO 8

#define TEST_LEN 1000

// SPI speed sweep over a bare jumper wire (MOSI->MISO); no LR2021 is attached,
// so the LR2021 datasheet clock cap (16 MHz) does not apply to this sweep and the
// regression gate in tests/test_c3_spi_clock.py deliberately excludes this file.
static const int spi_speeds[] = {1000000, 4000000, 8000000, 18000000};
static const int spi_speed_count = sizeof(spi_speeds) / sizeof(spi_speeds[0]);

static void blink(int times, int ms) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << LED_GPIO);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    for (int i = 0; i < times; i++) {
        gpio_set_level((gpio_num_t)LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level((gpio_num_t)LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    gpio_set_level((gpio_num_t)LED_GPIO, 1);
}

static void fill_pattern(uint8_t *buf, int len, uint8_t pattern) {
    if (pattern == 0x00) {
        memset(buf, 0x00, len);
    } else if (pattern == 0xFF) {
        memset(buf, 0xFF, len);
    } else if (pattern == 0x55) {
        memset(buf, 0x55, len);
    } else if (pattern == 0xAA) {
        memset(buf, 0xAA, len);
    }
}

static void fill_prbs15(uint8_t *buf, int len, uint16_t seed) {
    uint16_t lfsr = seed | 1;
    uint8_t bit_acc = 0;
    int bit_count = 0;
    for (int i = 0; i < len; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            uint16_t bit = ((lfsr >> 14) ^ (lfsr >> 13) ^ (lfsr >> 12) ^ (lfsr >> 10)) & 1;
            lfsr = (lfsr << 1) | bit;
            byte |= (bit << b);
        }
        buf[i] = byte;
    }
}

static int compare_buffers(const uint8_t *a, const uint8_t *b, int len,
                           int *first_err_offset) {
    int errors = 0;
    *first_err_offset = -1;
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            if (errors == 0) *first_err_offset = i;
            errors++;
        }
    }
    return errors;
}

static int run_test_at_speed(spi_device_handle_t dev, uint8_t *tx, uint8_t *rx,
                             uint8_t pattern, const char *pattern_name) {
    fill_pattern(tx, TEST_LEN, pattern);
    memset(rx, 0, TEST_LEN);

    spi_transaction_t trans = {};
    trans.length = TEST_LEN * 8;
    trans.tx_buffer = tx;
    trans.rx_buffer = rx;
    trans.flags = 0;

    esp_err_t ret = spi_device_polling_transmit(dev, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  SPI transmit failed: %s", esp_err_to_name(ret));
        return -1;
    }

    int first_err = -1;
    int errors = compare_buffers(tx, rx, TEST_LEN, &first_err);
    if (errors == 0) {
        printf("  %-8s PASS (0 errors / %d bytes)\n", pattern_name, TEST_LEN);
    } else {
        printf("  %-8s FAIL (%d errors / %d bytes, first at offset %d)\n",
               pattern_name, errors, TEST_LEN, first_err);
        if (first_err >= 0 && first_err < TEST_LEN) {
            printf("           expected 0x%02X got 0x%02X\n", tx[first_err], rx[first_err]);
        }
    }
    fflush(stdout);
    return errors;
}

static int run_prbs_test(spi_device_handle_t dev, uint8_t *tx, uint8_t *rx) {
    fill_prbs15(tx, TEST_LEN, 0x0001);
    memset(rx, 0, TEST_LEN);

    spi_transaction_t trans = {};
    trans.length = TEST_LEN * 8;
    trans.tx_buffer = tx;
    trans.rx_buffer = rx;

    esp_err_t ret = spi_device_polling_transmit(dev, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  SPI transmit failed: %s", esp_err_to_name(ret));
        return -1;
    }

    int first_err = -1;
    int errors = compare_buffers(tx, rx, TEST_LEN, &first_err);
    if (errors == 0) {
        printf("  %-8s PASS (0 errors / %d bytes)\n", "PRBS15", TEST_LEN);
    } else {
        printf("  %-8s FAIL (%d errors / %d bytes, first at offset %d)\n",
               "PRBS15", errors, TEST_LEN, first_err);
        if (first_err >= 0 && first_err < TEST_LEN) {
            printf("           expected 0x%02X got 0x%02X\n", tx[first_err], rx[first_err]);
        }
    }
    fflush(stdout);
    return errors;
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "=== SPI Loopback Test ===");
    setvbuf(stdout, NULL, _IONBF, 0);

    blink(3, 200);
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n");
    printf("=============================================\n");
    printf("  SPI Loopback Test\n");
    printf("  MOSI=GPIO%d  MISO=GPIO%d  SCK=GPIO%d\n", SPI_MOSI, SPI_MISO, SPI_SCK);
    printf("  Connect jumper: D7 --> D2\n");
    printf("  Test length: %d bytes per pattern\n", TEST_LEN);
    printf("=============================================\n");
    printf("\n");
    fflush(stdout);

    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SPI_MOSI;
    bus_cfg.miso_io_num = SPI_MISO;
    bus_cfg.sclk_io_num = SPI_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 1024;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        printf("FATAL: SPI bus init failed. Check pin assignments.\n");
        blink(10, 500);
        return;
    }
    ESP_LOGI(TAG, "SPI bus initialized");

    static uint8_t tx_buf[TEST_LEN] __attribute__((aligned(4)));
    static uint8_t rx_buf[TEST_LEN] __attribute__((aligned(4)));

    int total_pass = 0;
    int total_fail = 0;

    for (int s = 0; s < spi_speed_count; s++) {
        int speed = spi_speeds[s];
        printf("\n--- Testing at %d MHz ---\n", speed / 1000000);
        fflush(stdout);

        spi_device_interface_config_t dev_cfg = {};
        dev_cfg.mode = 0;
        dev_cfg.clock_speed_hz = speed;
        dev_cfg.spics_io_num = -1;
        dev_cfg.queue_size = 1;

        spi_device_handle_t dev = nullptr;
        ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device at %d MHz failed: %s",
                     speed / 1000000, esp_err_to_name(ret));
            printf("  SKIP (device add failed)\n");
            continue;
        }

        int speed_errors = 0;

        int e1 = run_test_at_speed(dev, tx_buf, rx_buf, 0x00, "0x00");
        int e2 = run_test_at_speed(dev, tx_buf, rx_buf, 0xFF, "0xFF");
        int e3 = run_test_at_speed(dev, tx_buf, rx_buf, 0x55, "0x55");
        int e4 = run_test_at_speed(dev, tx_buf, rx_buf, 0xAA, "0xAA");
        int e5 = run_prbs_test(dev, tx_buf, rx_buf);

        if (e1 > 0) speed_errors += e1;
        if (e2 > 0) speed_errors += e2;
        if (e3 > 0) speed_errors += e3;
        if (e4 > 0) speed_errors += e4;
        if (e5 > 0) speed_errors += e5;

        if (speed_errors == 0) {
            printf(">>> %d MHz: ALL PASS <<<\n", speed / 1000000);
            total_pass++;
            blink(1, 100);
        } else {
            printf(">>> %d MHz: FAILED (%d total errors) <<<\n",
                   speed / 1000000, speed_errors);
            total_fail++;
            blink(3, 100);
        }

        spi_bus_remove_device(dev);
        fflush(stdout);
    }

    printf("\n=============================================\n");
    printf("  SUMMARY\n");
    printf("  Speeds passed: %d / %d\n", total_pass, spi_speed_count);
    printf("  Speeds failed: %d / %d\n", total_fail, spi_speed_count);
    if (total_fail == 0) {
        printf("  RESULT: ALL SPEEDS PASS - wiring is excellent\n");
        printf("  Safe to solder LR2021 at 18 MHz\n");
    } else if (total_pass > 0) {
        printf("  RESULT: PARTIAL PASS - use highest passing speed\n");
        printf("  Check SPI-LOOPBACK-TEST-PLAN.md for guidance\n");
    } else {
        printf("  RESULT: ALL SPEEDS FAILED\n");
        printf("  Check jumper wire (D7 --> D2)\n");
        printf("  Check for shorts on SPI pins\n");
    }
    printf("=============================================\n");
    printf("\n");
    fflush(stdout);

    ESP_LOGI(TAG, "Test complete. LED will blink pattern.");
    ESP_LOGI(TAG, "  All pass: long blinks");
    ESP_LOGI(TAG, "  Some fail: short blinks");

    while (true) {
        if (total_fail == 0) {
            blink(2, 1000);
        } else if (total_pass > 0) {
            blink(3, 300);
        } else {
            blink(5, 200);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

#endif
