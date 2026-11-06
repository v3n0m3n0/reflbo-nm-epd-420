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
    uint32_t waited_us = 0;
    uint32_t max_us = timeout_ms * 1000;
    while (gpio_get_level(BOARD_PIN_LORA_BUSY) == 1) {
        esp_rom_delay_us(50);
        waited_us += 50;
        if (waited_us >= max_us) {
            return false;
        }
    }
    return true;
}

static esp_err_t lora_write_cmd(uint8_t cmd, const uint8_t *params, size_t len)
{
    if (!lora_wait_busy(50)) {
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
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    return err;
}

static esp_err_t lora_read_cmd(uint8_t cmd, uint8_t *out, size_t len)
{
    if (!lora_wait_busy(50)) {
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
        uint8_t zeros[8] = {0};
        spi_transaction_t t2 = {
            .length = 8 * len,
            .tx_buffer = (len <= sizeof(zeros)) ? zeros : NULL,
            .rx_buffer = out,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t2);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    return err;
}

static void IRAM_ATTR lora_dio1_isr_handler(void *arg)
{
    TaskHandle_t task = (TaskHandle_t)arg;
    if (task != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void lora_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LoRa SX1262 monitor task started (interrupt-driven)");

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_stats.enabled || !s_stats.initialized) {
            continue;
        }

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
            int16_t rssi = -120;
            int8_t snr = 0;
            if (lora_read_cmd(0x14, pkt_stat, 3) == ESP_OK) {
                rssi = -(int16_t)(pkt_stat[0] / 2);
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
                     (unsigned)s_stats.packet_count, (int)rssi, (int)snr);
        } else if (irq != 0) {
            /* Clear non-RxDone IRQ flags */
            uint8_t clr[2] = { irq_stat[0], irq_stat[1] };
            lora_write_cmd(0x02, clr, 2);
        }

        /* If DIO1 is still high (e.g. back-to-back packet arrived), re-trigger processing */
        if (gpio_get_level(BOARD_PIN_LORA_DIO1) == 1) {
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
    }
}

esp_err_t lora_widget_apply(const lora_config_t *cfg)
{
    if (s_stats_mutex == NULL) {
        s_stats_mutex = xSemaphoreCreateMutex();
    }

    if (cfg != NULL && s_stats.initialized && s_stats.enabled == cfg->enabled &&
        s_stats.freq_khz == cfg->freq_khz && s_stats.sf == cfg->sf &&
        s_stats.bw_khz == cfg->bw_khz && s_stats.cr == cfg->cr &&
        s_stats.sync_word == cfg->sync_word) {
        /* Radio parameters unchanged; only update wake_on_rx */
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.wake_on_rx = cfg->wake_on_rx;
            xSemaphoreGive(s_stats_mutex);
        }
        return ESP_OK;
    }

    /* Pause RX and disable interrupts during reconfiguration */
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);
    if (s_task != NULL) {
        ulTaskNotifyTake(pdTRUE, 0); /* drain any stale notifications */
    }

    if (cfg == NULL || !cfg->enabled) {
        if (s_stats.initialized && s_lora_spi != NULL) {
            uint8_t sleep_cfg = 0x00; /* cold start, config not retained (0x84) */
            lora_write_cmd(0x84, &sleep_cfg, 1);
        }
        /* Bring outputs low to prevent parasitic ESD leakage into unpowered chip */
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);

        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.enabled = false;
            s_stats.initialized = false;
            if (cfg != NULL) {
                s_stats.freq_khz = cfg->freq_khz;
                s_stats.sf = cfg->sf;
                s_stats.bw_khz = cfg->bw_khz;
                s_stats.cr = cfg->cr;
                s_stats.sync_word = cfg->sync_word;
                s_stats.wake_on_rx = cfg->wake_on_rx;
            }
            xSemaphoreGive(s_stats_mutex);
        }
        ESP_LOGI(TAG, "LoRa radio disabled");
        return ESP_OK;
    }

    /* Mark uninitialized while reconfiguring */
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.initialized = false;
        xSemaphoreGive(s_stats_mutex);
    }

    /* GPIO configuration for outputs (NSS, RST, EN) */
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_NSS) | (1ULL << BOARD_PIN_LORA_RST) | (1ULL << BOARD_PIN_LORA_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "output gpio");
    gpio_set_level(BOARD_PIN_LORA_EN, 1);
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    gpio_set_level(BOARD_PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* GPIO configuration for inputs: BUSY (polling only, NO interrupt) */
    gpio_config_t busy_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* pull down so unpopulated bus stays LOW */
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_conf), TAG, "busy gpio");

    /* GPIO configuration for DIO1: rising edge interrupt */
    gpio_config_t dio1_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_DIO1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dio1_conf), TAG, "dio1 gpio");

    /* Hardware reset */
    gpio_set_level(BOARD_PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!lora_wait_busy(50)) {
        ESP_LOGW(TAG, "SX1262 BUSY timeout after reset (LoRa module not populated or unresponsive) - disabled");
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.enabled = true;
            s_stats.initialized = false;
            xSemaphoreGive(s_stats_mutex);
        }
        return ESP_OK;
    }

    /* SPI bus initialization on SPI3 */
    if (s_lora_spi == NULL) {
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
    }

    /* Configure SX1262 */
    uint8_t standby_rc = 0x00; /* STDBY_RC (0x80) */
    if (lora_write_cmd(0x80, &standby_rc, 1) != ESP_OK) {
        ESP_LOGW(TAG, "SX1262 SetStandby failed (module not populated) - disabled");
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.enabled = true;
            s_stats.initialized = false;
            xSemaphoreGive(s_stats_mutex);
        }
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

    /* Calibrate image for target band (0x98) */
    uint8_t cal_img[2];
    if (cfg->freq_khz >= 430000 && cfg->freq_khz <= 440000) {
        cal_img[0] = 0x6B; cal_img[1] = 0x6F;
    } else if (cfg->freq_khz >= 470000 && cfg->freq_khz <= 510000) {
        cal_img[0] = 0x75; cal_img[1] = 0x81;
    } else if (cfg->freq_khz >= 779000 && cfg->freq_khz <= 787000) {
        cal_img[0] = 0xC1; cal_img[1] = 0xC5;
    } else if (cfg->freq_khz >= 902000 && cfg->freq_khz <= 928000) {
        cal_img[0] = 0xE1; cal_img[1] = 0xE5;
    } else {
        /* Default 863-870 MHz */
        cal_img[0] = 0xD7; cal_img[1] = 0xDB;
    }
    lora_write_cmd(0x98, cal_img, 2);

    uint8_t pkt_type = 0x01; /* Packet Type LoRa (0x8A) */
    lora_write_cmd(0x8A, &pkt_type, 1);

    /* Frequency: (freq_khz * 1000) / (32000000 / 2^25) = freq_khz * 1000 * 33554432 / 32000000 */
    uint32_t f_rf = (uint32_t)(((uint64_t)cfg->freq_khz * 1000ULL * (1ULL << 25)) / 32000000ULL);
    uint8_t freq[4] = {
        (uint8_t)((f_rf >> 24) & 0xFF),
        (uint8_t)((f_rf >> 16) & 0xFF),
        (uint8_t)((f_rf >> 8) & 0xFF),
        (uint8_t)(f_rf & 0xFF)
    };
    lora_write_cmd(0x86, freq, 4);

    /* Buffer base address: TX 0, RX 0 (0x8F) */
    uint8_t base_addr[2] = { 0x00, 0x00 };
    lora_write_cmd(0x8F, base_addr, 2);

    /* Modulation: SF (7..12), BW (125=0x04, 250=0x05, 500=0x06), CR (4/5=1..4/8=4), LDRO (0x8B) */
    uint8_t sf = cfg->sf >= 7 && cfg->sf <= 12 ? cfg->sf : 7;
    uint8_t bw = cfg->bw_khz == 125 ? 0x04 : (cfg->bw_khz == 500 ? 0x06 : 0x05);
    uint8_t cr = cfg->cr >= 5 && cfg->cr <= 8 ? (cfg->cr - 4) : (cfg->cr >= 1 && cfg->cr <= 4 ? cfg->cr : 1);
    uint16_t bw_val = (cfg->bw_khz == 125 || cfg->bw_khz == 500) ? cfg->bw_khz : 250;
    /* Auto compute LowDataRateOptimize: Ts = 2^SF / BW_kHz > 16.38 ms <=> (2^SF * 100) > (1638 * BW_kHz) */
    uint8_t ldro = (((uint32_t)(1u << sf) * 100u) > (1638u * bw_val)) ? 0x01 : 0x00;
    uint8_t mod_params[4] = { sf, bw, cr, ldro };
    lora_write_cmd(0x8B, mod_params, 4);

    /* Set SyncWord: write registers 0x0740, 0x0741 via WriteRegister (0x0D) */
    uint16_t sw = cfg->sync_word;
    uint8_t sync_data[4] = { 0x07, 0x40, (uint8_t)((sw >> 8) & 0xFF), (uint8_t)(sw & 0xFF) };
    lora_write_cmd(0x0D, sync_data, 4);

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

    /* Launch background receiver task if not created */
    if (s_task == NULL) {
        xTaskCreate(lora_rx_task, "lora_rx", 3072, NULL, 5, &s_task);
    }

    /* Configure and hook DIO1 interrupt */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_err));
    }
    gpio_isr_handler_remove(BOARD_PIN_LORA_DIO1);
    gpio_isr_handler_add(BOARD_PIN_LORA_DIO1, lora_dio1_isr_handler, s_task);
    gpio_intr_enable(BOARD_PIN_LORA_DIO1);

    if (gpio_get_level(BOARD_PIN_LORA_DIO1) == 1 && s_task != NULL) {
        xTaskNotifyGive(s_task);
    }

    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.initialized = true;
        s_stats.enabled = true;
        s_stats.freq_khz = cfg->freq_khz;
        s_stats.sf = sf;
        s_stats.bw_khz = bw_val;
        s_stats.cr = cfg->cr >= 5 && cfg->cr <= 8 ? cfg->cr : 5;
        s_stats.sync_word = sw;
        s_stats.wake_on_rx = cfg->wake_on_rx;
        xSemaphoreGive(s_stats_mutex);
    }

    ESP_LOGI(TAG, "SX1262 LoRa configured: %lu kHz, SF%u, BW%u kHz, CR 4/%u, Sync 0x%04X, LDRO %u",
             (unsigned long)cfg->freq_khz, (unsigned)sf, (unsigned)bw_val, (unsigned)s_stats.cr,
             (unsigned)sw, (unsigned)ldro);
    return ESP_OK;
}

esp_err_t lora_widget_init(void)
{
    lora_config_t def = {
        .enabled = true,
        .freq_khz = 868000,
        .sf = 7,
        .bw_khz = 250,
        .cr = 5,
        .sync_word = 0x1424,
        .wake_on_rx = false,
    };
    return lora_widget_apply(&def);
}

void lora_widget_sleep(void)
{
    if (s_stats.enabled && s_stats.wake_on_rx) {
        return;
    }
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);
    if (s_stats.initialized && s_lora_spi != NULL) {
        uint8_t sleep_cfg = 0x00; /* cold start, config not retained (0x84) */
        lora_write_cmd(0x84, &sleep_cfg, 1);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);
    gpio_set_level(BOARD_PIN_LORA_RST, 0);
    gpio_set_level(BOARD_PIN_LORA_EN, 0);

    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.initialized = false;
        xSemaphoreGive(s_stats_mutex);
    }
}

void lora_widget_notify(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void lora_widget_reset_stats(void)
{
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.packet_count = 0;
        s_stats.last_rssi_dbm = 0;
        s_stats.last_snr_db = 0;
        s_stats.last_packet_time_s = 0;
        memset(s_stats.last_sender, 0, sizeof(s_stats.last_sender));
        xSemaphoreGive(s_stats_mutex);
    }
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
