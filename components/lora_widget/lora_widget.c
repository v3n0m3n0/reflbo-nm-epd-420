#include "lora_widget.h"

#include <string.h>
#include <time.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lora_widget";

static spi_device_handle_t s_lora_spi;
static lora_stats_t s_stats;
static SemaphoreHandle_t s_stats_mutex;
static TaskHandle_t s_task;

static bool lora_wait_busy(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (gpio_get_level(BOARD_PIN_LORA_BUSY) == 1) {
        vTaskDelay(pdMS_TO_TICKS(1));
        waited++;
        if (waited >= timeout_ms) {
            return false;
        }
    }
    return true;
}

static esp_err_t lora_write_cmd(uint8_t cmd, const uint8_t *params, size_t len)
{
    if (!lora_wait_busy(30)) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t);
    if (err == ESP_OK && len > 0 && params != NULL) {
        spi_transaction_t t2 = {
            .length = 8 * len,
            .tx_buffer = params,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t2);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    lora_wait_busy(30);
    return err;
}

static esp_err_t lora_read_cmd(uint8_t cmd, uint8_t *out, size_t len)
{
    if (!lora_wait_busy(30)) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t);

    /* Read status byte */
    uint8_t dummy = 0;
    spi_transaction_t td = {
        .length = 8,
        .tx_buffer = &dummy,
    };
    err = spi_device_polling_transmit(s_lora_spi, &td);

    if (err == ESP_OK && len > 0 && out != NULL) {
        spi_transaction_t t2 = {
            .length = 8 * len,
            .rx_buffer = out,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t2);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    lora_wait_busy(30);
    return err;
}

static void lora_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LoRa SX1262 monitor task started (EU868)");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Read IRQ status (cmd 0x12) */
        uint8_t irq_stat[2] = {0};
        if (lora_read_cmd(0x12, irq_stat, 2) != ESP_OK) {
            continue;
        }

        uint16_t irq = ((uint16_t)irq_stat[0] << 8) | irq_stat[1];

        /* Bit 1 is RxDone (0x0002) */
        if (irq & 0x0002) {
            /* GetPacketStatus (cmd 0x14) */
            uint8_t pkt_stat[3] = {0};
            int8_t rssi = -120;
            int8_t snr = 0;
            if (lora_read_cmd(0x14, pkt_stat, 3) == ESP_OK) {
                rssi = -(int8_t)(pkt_stat[0] / 2);
                snr = ((int8_t)pkt_stat[1]) / 4;
            }

            /* Clear IRQ (cmd 0x02, 0x03FF to clear all pending flags) */
            uint8_t clr[2] = { 0x03, 0xFF };
            lora_write_cmd(0x02, clr, 2);

            /* Update stats */
            if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_stats.packet_count++;
                s_stats.last_rssi_dbm = rssi;
                s_stats.last_snr_db = snr;
                s_stats.last_packet_time_s = (uint32_t)time(NULL);
                xSemaphoreGive(s_stats_mutex);
            }

            ESP_LOGI(TAG, "LoRa packet received! Total: %u, RSSI: %d dBm, SNR: %d dB",
                     (unsigned)s_stats.packet_count, rssi, snr);
        } else if (irq != 0) {
            /* Clear non-RxDone IRQ flags */
            uint8_t clr[2] = { irq_stat[0], irq_stat[1] };
            lora_write_cmd(0x02, clr, 2);
        }
    }
}

esp_err_t lora_widget_init(void)
{
    if (s_stats_mutex == NULL) {
        s_stats_mutex = xSemaphoreCreateMutex();
    }

    /* GPIO configuration */
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_NSS) | (1ULL << BOARD_PIN_LORA_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "output gpio");
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    gpio_set_level(BOARD_PIN_LORA_RST, 1);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_BUSY) | (1ULL << BOARD_PIN_LORA_DIO1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* pull down so unpopulated bus stays LOW */
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_conf), TAG, "input gpio");

    /* Hardware reset */
    gpio_set_level(BOARD_PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!lora_wait_busy(30)) {
        ESP_LOGW(TAG, "SX1262 BUSY timeout after reset (LoRa module not populated or unresponsive) - disabled");
        return ESP_OK;
    }

    /* SPI bus initialization on SPI3 */
    spi_bus_config_t buscfg = {
        .miso_io_num = BOARD_PIN_LORA_MISO,
        .mosi_io_num = BOARD_PIN_LORA_MOSI,
        .sclk_io_num = BOARD_PIN_LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1, /* manual CS via NSS GPIO */
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &s_lora_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure SX1262 for EU868 reception */
    uint8_t standby_rc = 0x00; /* STDBY_RC (0x80) */
    if (lora_write_cmd(0x80, &standby_rc, 1) != ESP_OK) {
        ESP_LOGW(TAG, "SX1262 SetStandby failed (module not populated) - disabled");
        return ESP_OK;
    }

    uint8_t reg_mode = 0x01; /* DC-DC mode (0x96) */
    lora_write_cmd(0x96, &reg_mode, 1);

    uint8_t dio2_rf_sw = 0x01; /* DIO2 as RF Switch (0x9D) */
    lora_write_cmd(0x9D, &dio2_rf_sw, 1);

    /* TCXO 1.8V, delay ~5ms: 320 * 15.625us = 0x000140 (0x97) */
    uint8_t tcxo_cfg[4] = { 0x02, 0x00, 0x01, 0x40 };
    lora_write_cmd(0x97, tcxo_cfg, 4);

    /* Calibrate all blocks (0x89) */
    uint8_t cal_all = 0x7F;
    lora_write_cmd(0x89, &cal_all, 1);

    /* Calibrate image for 863-870 MHz (0x98) */
    uint8_t cal_img[2] = { 0xD7, 0xDB };
    lora_write_cmd(0x98, cal_img, 2);

    uint8_t pkt_type = 0x01; /* Packet Type LoRa (0x8A) */
    lora_write_cmd(0x8A, &pkt_type, 1);

    /* Frequency 868.0 MHz: 868000000 / (32000000 / 2^25) = 0x36400000 (0x86) */
    uint8_t freq[4] = { 0x36, 0x40, 0x00, 0x00 };
    lora_write_cmd(0x86, freq, 4);

    /* Buffer base address: TX 0, RX 0 (0x8F) */
    uint8_t base_addr[2] = { 0x00, 0x00 };
    lora_write_cmd(0x8F, base_addr, 2);

    /* Modulation: SF7 (0x07), BW250kHz (0x05), CR 4/5 (0x01), LowDataRateOpt 0x00 (0x8B) */
    uint8_t mod_params[4] = { 0x07, 0x05, 0x01, 0x00 };
    lora_write_cmd(0x8B, mod_params, 4);

    /* Packet params: preamble=12, explicit header, max len=255, crc on, standard IQ (0x8C) */
    uint8_t pkt_params[6] = { 0x00, 0x0C, 0x00, 0xFF, 0x01, 0x00 };
    lora_write_cmd(0x8C, pkt_params, 6);

    /* SetDioIrqParams: RxDone(0x0002) | Timeout(0x0200) | CrcErr(0x0040) = 0x0242, DIO1 mask = 0x0002 (0x08) */
    uint8_t dio_irq[8] = { 0x02, 0x42, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
    lora_write_cmd(0x08, dio_irq, 8);

    /* Clear IRQ (0x02) */
    uint8_t clr_irq[2] = { 0x03, 0xFF };
    lora_write_cmd(0x02, clr_irq, 2);

    /* Arm continuous RX (0x82) */
    uint8_t rx_timeout[3] = { 0xFF, 0xFF, 0xFF };
    lora_write_cmd(0x82, rx_timeout, 3);

    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.initialized = true;
        xSemaphoreGive(s_stats_mutex);
    }

    xTaskCreate(lora_rx_task, "lora_rx", 3072, NULL, 5, &s_task);
    ESP_LOGI(TAG, "SX1262 LoRa receiver initialized successfully");
    return ESP_OK;
}

void lora_widget_sleep(void)
{
    if (!s_stats.initialized) return;
    uint8_t sleep_cfg = 0x00; /* cold start, config not retained (0x84) */
    lora_write_cmd(0x84, &sleep_cfg, 1);
    gpio_set_level(BOARD_PIN_LORA_EN, 0);
}

lora_stats_t lora_widget_get_stats(void)
{
    lora_stats_t copy = {0};
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        copy = s_stats;
        xSemaphoreGive(s_stats_mutex);
    }
    return copy;
}
