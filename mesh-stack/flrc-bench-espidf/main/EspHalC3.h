#pragma once

#include <RadioLib.h>

#if CONFIG_IDF_TARGET_ESP32C3 == 0
#error This HAL only supports ESP32-C3 targets
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_log.h"

#define LOW    (0x0)
#define HIGH   (0x1)
#define INPUT  (0x01)
#define OUTPUT (0x03)
#define RISING (0x01)
#define FALLING (0x02)
#define NOP()  asm volatile ("nop")

// SPI clock for the LR2021 radio on GPSPI2 (SPI2_HOST / GPSPI2). 16 MHz is the
// LR2021 datasheet maximum SPI clock; staying inside spec improves reliability
// at range and across temperature/voltage corners. The previous hard-coded
// 18 MHz ran at bench distance but was above spec (same defect class as
// balloon-fresh P0.4 t_0b2f5534, whose GDMA HAL sat at 40 MHz).
#define ESPHAL_C3_SPI_HZ   (16 * 1000 * 1000)


class EspHalC3 : public RadioLibHal {
  public:
    EspHalC3(int8_t sck, int8_t miso, int8_t mosi)
      : RadioLibHal(INPUT, OUTPUT, LOW, HIGH, RISING, FALLING),
      spiSCK(sck), spiMISO(miso), spiMOSI(mosi), csPin(-1), busyPin(-1) {
    }

    void init() override {
      spiBegin();
    }

    void term() override {
      spiEnd();
    }

    void pinMode(uint32_t pin, uint32_t mode) override {
      if(pin == RADIOLIB_NC) return;
      gpio_config_t conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = (gpio_mode_t)mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
      };
      gpio_config(&conf);
    }

    void digitalWrite(uint32_t pin, uint32_t value) override {
      if(pin == RADIOLIB_NC) return;
      gpio_set_level((gpio_num_t)pin, value);
    }

    uint32_t digitalRead(uint32_t pin) override {
      if(pin == RADIOLIB_NC) return(0);
      return(gpio_get_level((gpio_num_t)pin));
    }

    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override {
      if(interruptNum == RADIOLIB_NC) return;
      if(!this->isrInstalled) {
        gpio_install_isr_service((int)ESP_INTR_FLAG_IRAM);
        this->isrInstalled = true;
      }
      gpio_set_intr_type((gpio_num_t)interruptNum, (gpio_int_type_t)(mode & 0x7));
      gpio_isr_handler_add((gpio_num_t)interruptNum, (void (*)(void*))interruptCb, NULL);
    }

    void detachInterrupt(uint32_t interruptNum) override {
      if(interruptNum == RADIOLIB_NC) return;
      gpio_isr_handler_remove((gpio_num_t)interruptNum);
      gpio_wakeup_disable((gpio_num_t)interruptNum);
      gpio_set_intr_type((gpio_num_t)interruptNum, GPIO_INTR_DISABLE);
    }

    void delay(unsigned long ms) override {
      vTaskDelay(ms / portTICK_PERIOD_MS);
    }

    void delayMicroseconds(unsigned long us) override {
      uint64_t m = (uint64_t)esp_timer_get_time();
      if(us) {
        uint64_t e = (m + us);
        if(m > e) {
          while((uint64_t)esp_timer_get_time() > e) { NOP(); }
        }
        while((uint64_t)esp_timer_get_time() < e) { NOP(); }
      }
    }

    unsigned long millis() override {
      return((unsigned long)(esp_timer_get_time() / 1000ULL));
    }

    unsigned long micros() override {
      return((unsigned long)(esp_timer_get_time()));
    }

    long pulseIn(uint32_t pin, uint32_t state, unsigned long timeout) override {
      if(pin == RADIOLIB_NC) return(0);
      this->pinMode(pin, INPUT);
      uint32_t start = this->micros();
      uint32_t curtick = this->micros();
      while(this->digitalRead(pin) == state) {
        if((this->micros() - curtick) > timeout) return(0);
      }
      return(this->micros() - start);
    }

    void spiBegin() {
      if (this->spiInitialized) return;
      spi_bus_config_t bus_cfg = {};
      bus_cfg.mosi_io_num = this->spiMOSI;
      bus_cfg.miso_io_num = this->spiMISO;
      bus_cfg.sclk_io_num = this->spiSCK;
      bus_cfg.quadwp_io_num = -1;
      bus_cfg.quadhd_io_num = -1;
      bus_cfg.max_transfer_sz = 512;
      esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
      if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("HAL", "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return;
      }

      spi_device_interface_config_t dev_cfg = {};
      dev_cfg.mode = 0;
      dev_cfg.clock_speed_hz = ESPHAL_C3_SPI_HZ;   // 16 MHz (LR2021 datasheet max)
      dev_cfg.spics_io_num = -1;
      dev_cfg.queue_size = 1;
      ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &this->spiDev);
      if (ret != ESP_OK) {
        ESP_LOGE("HAL", "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return;
      }
      this->spiInitialized = true;
      ESP_LOGI("HAL", "SPI initialized: MOSI=%d MISO=%d SCK=%d", this->spiMOSI, this->spiMISO, this->spiSCK);
    }

    void spiBeginTransaction() {}

    uint8_t spiTransferByte(uint8_t b) {
      spi_transaction_t trans = {};
      trans.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
      trans.length = 8;
      trans.tx_data[0] = b;
      esp_err_t ret = spi_device_polling_transmit(this->spiDev, &trans);
      if (ret != ESP_OK) {
        ESP_LOGE("HAL", "spiTransferByte failed: %s", esp_err_to_name(ret));
        return 0xFF;
      }
      return trans.rx_data[0];
    }

    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) {
      if (len == 0) return;
      spi_transaction_t trans = {};
      trans.length = len * 8;
      trans.tx_buffer = out;
      trans.rx_buffer = in;
      esp_err_t ret = spi_device_polling_transmit(this->spiDev, &trans);
      if (ret != ESP_OK) {
        ESP_LOGE("HAL", "spiTransfer failed: %s", esp_err_to_name(ret));
        memset(in, 0xFF, len);
      }
    }

    void spiEndTransaction() {}

    void spiEnd() {
      if (this->spiDev) {
        spi_bus_remove_device(this->spiDev);
        this->spiDev = nullptr;
      }
      spi_bus_free(SPI2_HOST);
      this->spiInitialized = false;
    }

    void setCsPin(int8_t pin) { this->csPin = pin; }
    void setBusyPin(int8_t pin) { this->busyPin = pin; }

  private:
    int8_t spiSCK;
    int8_t spiMISO;
    int8_t spiMOSI;
    int8_t csPin;
    int8_t busyPin;
    spi_device_handle_t spiDev = nullptr;
    bool spiInitialized = false;
    bool isrInstalled = false;
};
