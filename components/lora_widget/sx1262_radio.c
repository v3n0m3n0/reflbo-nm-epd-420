#include "sx1262_radio.h"

#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "meshcore_proto.h"

static const char *TAG = "sx1262_radio";

static spi_device_handle_t s_lora_spi = NULL;

bool sx1262_radio_is_ready(void)
{
    return s_lora_spi != NULL;
}

static bool lora_wait_busy(uint32_t timeout_ms)
{
    uint32_t waited_us = 0;
    uint32_t max_us = timeout_ms * 1000U;
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
    if (!s_lora_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!board_spi3_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!lora_wait_busy(50)) {
        board_spi3_unlock();
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
    board_spi3_unlock();
    return err;
}

static esp_err_t lora_read_cmd(uint8_t cmd, uint8_t *out, size_t len)
{
    if (!s_lora_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!board_spi3_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!lora_wait_busy(50)) {
        board_spi3_unlock();
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
    if (err == ESP_OK) {
        err = spi_device_polling_transmit(s_lora_spi, &td);
    }

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
    board_spi3_unlock();
    return err;
}

static esp_err_t lora_write_reg(uint16_t reg, uint8_t val)
{
    uint8_t data[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return lora_write_cmd(0x0D, data, 3);
}

static esp_err_t lora_write_buffer(uint8_t offset, const uint8_t *data, size_t len)
{
    if (!s_lora_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!board_spi3_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!lora_wait_busy(50)) {
        board_spi3_unlock();
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    uint8_t cmd_hdr[2] = { 0x0E, offset };
    spi_transaction_t t_hdr = {
        .length = 16,
        .tx_buffer = cmd_hdr,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t_hdr);
    if (err == ESP_OK && len > 0 && data != NULL) {
        spi_transaction_t t_data = {
            .length = 8 * len,
            .tx_buffer = data,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t_data);
    }

    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    board_spi3_unlock();
    return err;
}

static esp_err_t lora_read_buffer(uint8_t offset, uint8_t *out, size_t len)
{
    if (!s_lora_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!board_spi3_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!lora_wait_busy(50)) {
        board_spi3_unlock();
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    uint8_t cmd_hdr[3] = { 0x1E, offset, 0x00 /* dummy */ };
    spi_transaction_t t_hdr = {
        .length = 24,
        .tx_buffer = cmd_hdr,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t_hdr);
    if (err == ESP_OK && len > 0 && out != NULL) {
        spi_transaction_t t_data = {
            .length = 8 * len,
            .tx_buffer = NULL,
            .rx_buffer = out,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t_data);
    }

    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    board_spi3_unlock();
    return err;
}

uint8_t sx1262_bw_register(uint32_t bw_hz)
{
    if (bw_hz <= 7800)   return 0x00; /* 7.81 kHz */
    if (bw_hz <= 10400)  return 0x08; /* 10.42 kHz */
    if (bw_hz <= 15600)  return 0x01; /* 15.63 kHz */
    if (bw_hz <= 20800)  return 0x09; /* 20.83 kHz */
    if (bw_hz <= 31250)  return 0x02; /* 31.25 kHz */
    if (bw_hz <= 41700)  return 0x0A; /* 41.67 kHz */
    if (bw_hz <= 62500)  return 0x03; /* 62.50 kHz */
    if (bw_hz <= 125000) return 0x04; /* 125.00 kHz */
    if (bw_hz <= 250000) return 0x05; /* 250.00 kHz */
    return 0x06;                       /* 500.00 kHz */
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

void sx1262_radio_power_off(TaskHandle_t rx_task, bool send_cold_sleep)
{
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);
    if (rx_task != NULL) {
        xTaskNotifyStateClear(rx_task);
    }
    if (send_cold_sleep && s_lora_spi != NULL) {
        uint8_t sleep_cfg = 0x00; /* cold start (0x84) */
        lora_write_cmd(0x84, &sleep_cfg, 1);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);
    gpio_set_level(BOARD_PIN_LORA_RST, 0);
    gpio_set_level(BOARD_PIN_LORA_EN, 0);
}

esp_err_t sx1262_radio_configure(const lora_config_t *cfg, TaskHandle_t rx_task,
                                 uint32_t *out_bw_hz, uint8_t *out_sf, uint8_t *out_cr,
                                 int8_t *out_pwr, uint16_t *out_sw, uint8_t *out_ldro,
                                 bool *out_hw_ok)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_hw_ok) *out_hw_ok = false;

    /* Pause RX and disable interrupts during reconfiguration */
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);
    if (rx_task != NULL) {
        xTaskNotifyStateClear(rx_task);
    }

    /* Initialize shared SPI3 bus once; board_spi3_init() configures TF_CS (GPIO7) & LORA_NSS (GPIO8) */
    if (s_lora_spi == NULL) {
        esp_err_t ret = board_spi3_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "board_spi3_init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = 4 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = -1,
            .queue_size = 1,
        };
        ret = spi_bus_add_device(SPI3_HOST, &devcfg, &s_lora_spi);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* Configure SX1262 control/status pins once (LORA_EN GPIO47 is owned by board_init; CS pins by board_spi3_init) */
    static bool s_pins_configured = false;
    if (!s_pins_configured) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = (1ULL << BOARD_PIN_LORA_RST),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t rc = gpio_config(&rst_conf);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "rst gpio: %s", esp_err_to_name(rc));
            return rc;
        }

        gpio_config_t busy_conf = {
            .pin_bit_mask = (1ULL << BOARD_PIN_LORA_BUSY),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        rc = gpio_config(&busy_conf);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "busy gpio: %s", esp_err_to_name(rc));
            return rc;
        }

        gpio_config_t dio1_conf = {
            .pin_bit_mask = (1ULL << BOARD_PIN_LORA_DIO1),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_POSEDGE,
        };
        rc = gpio_config(&dio1_conf);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "dio1 gpio: %s", esp_err_to_name(rc));
            return rc;
        }
        s_pins_configured = true;
    }

    gpio_set_level(BOARD_PIN_TF_CS, 1);
    gpio_set_level(BOARD_PIN_LORA_EN, 1);
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    gpio_set_level(BOARD_PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Hardware reset */
    gpio_set_level(BOARD_PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!lora_wait_busy(50)) {
        ESP_LOGW(TAG, "SX1262 BUSY timeout after reset - radio disabled");
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);
        return ESP_OK;
    }

    /* SetStandby STDBY_RC (0x80) */
    uint8_t standby_rc = 0x00;
    if (lora_write_cmd(0x80, &standby_rc, 1) != ESP_OK) {
        ESP_LOGW(TAG, "SX1262 SetStandby failed - radio disabled");
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);
        return ESP_OK;
    }

    uint8_t reg_mode = 0x01; /* DC-DC mode (0x96) */
    lora_write_cmd(0x96, &reg_mode, 1);

    uint8_t dio2_rf_sw = 0x01; /* DIO2 as RF switch (0x9D) */
    lora_write_cmd(0x9D, &dio2_rf_sw, 1);

    /* TCXO 1.8V, ~5ms delay: 0x02, 0x00, 0x01, 0x40 (0x97) */
    uint8_t tcxo_cfg[4] = { 0x02, 0x00, 0x01, 0x40 };
    lora_write_cmd(0x97, tcxo_cfg, 4);

    /* Calibrate all blocks (0x89) */
    uint8_t cal_all = 0x7F;
    lora_write_cmd(0x89, &cal_all, 1);

    /* Calibrate image for band (0x98) */
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
        cal_img[0] = 0xD7; cal_img[1] = 0xDB; /* Default 863-870 MHz */
    }
    lora_write_cmd(0x98, cal_img, 2);

    uint8_t pkt_type = 0x01; /* LoRa packet type (0x8A) */
    lora_write_cmd(0x8A, &pkt_type, 1);

    /* RF frequency (0x86) */
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

    /* SX1262 PA Configuration (0x95): +22 dBm power amplifier */
    uint8_t pa_config[4] = { 0x04, 0x07, 0x00, 0x01 };
    lora_write_cmd(0x95, pa_config, 4);

    /* Over Current Protection (OCP): 140 mA limit at register 0x08E7 (0x38) */
    lora_write_reg(0x08E7, 0x38);

    /* TX Parameters (0x8E): power clamp [-9..22], ramp 200 us (0x04) */
    int8_t pwr = cfg->tx_power_dbm;
    if (pwr < -9) pwr = -9;
    if (pwr > 22) pwr = 22;
    uint8_t tx_params[2] = { (uint8_t)pwr, 0x04 };
    lora_write_cmd(0x8E, tx_params, 2);

    /* RX Boosted gain: register 0x08AC -> 0x96 for boosted, 0x94 for power saving */
    lora_write_reg(0x08AC, cfg->rx_boosted ? 0x96 : 0x94);

    /* Modulation Parameters (0x8B): SF, BW, CR, LDRO */
    uint8_t sf = (cfg->sf >= 5 && cfg->sf <= 12) ? cfg->sf : 7;
    uint32_t bw_hz = cfg->bw_hz ? cfg->bw_hz : (uint32_t)(cfg->bw_khz ? cfg->bw_khz : 250) * 1000U;
    uint8_t bw_reg = sx1262_bw_register(bw_hz);
    uint8_t cr = (cfg->cr >= 5 && cfg->cr <= 8) ? cfg->cr : 5;
    uint8_t cr_reg = cr - 4;

    /* LowDataRateOptimize: Symbol time Ts = 2^SF / BW > 16.38 ms */
    uint8_t ldro = (((uint64_t)(1U << sf) * 1000000ULL / bw_hz) > 16380ULL) ? 0x01 : 0x00;
    uint8_t mod_params[4] = { sf, bw_reg, cr_reg, ldro };
    lora_write_cmd(0x8B, mod_params, 4);

    /* SyncWord: registers 0x0740, 0x0741 via WriteRegister (0x0D) */
    uint16_t sw = cfg->sync_word ? cfg->sync_word : 0x1424;
    lora_write_reg(0x0740, (uint8_t)((sw >> 8) & 0xFF));
    lora_write_reg(0x0741, (uint8_t)(sw & 0xFF));

    /* Packet Parameters (0x8C): adaptive preamble, explicit header, payload 255, crc on, standard IQ */
    uint16_t preamble = (sf <= 8) ? 32 : 16;
    uint8_t pkt_params[6] = {
        (uint8_t)(preamble >> 8),
        (uint8_t)(preamble & 0xFF),
        0x00, /* Explicit header */
        0xFF, /* Max RX payload */
        0x01, /* CRC on */
        0x00  /* Standard IQ */
    };
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

    /* Configure and hook DIO1 interrupt (ISR service already installed by board_init; fallback if absent) */
    esp_err_t isr_err = gpio_isr_handler_remove(BOARD_PIN_LORA_DIO1);
    if (isr_err == ESP_ERR_INVALID_STATE) {
        isr_err = gpio_install_isr_service(0);
        if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_err));
        }
    }
    isr_err = gpio_isr_handler_add(BOARD_PIN_LORA_DIO1, lora_dio1_isr_handler, rx_task);
    if (isr_err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_isr_handler_add: %s", esp_err_to_name(isr_err));
    }
    gpio_intr_enable(BOARD_PIN_LORA_DIO1);

    if (gpio_get_level(BOARD_PIN_LORA_DIO1) == 1 && rx_task != NULL) {
        xTaskNotifyGive(rx_task);
    }

    if (out_bw_hz) *out_bw_hz = bw_hz;
    if (out_sf)    *out_sf = sf;
    if (out_cr)    *out_cr = cr;
    if (out_pwr)   *out_pwr = pwr;
    if (out_sw)    *out_sw = sw;
    if (out_ldro)  *out_ldro = ldro;
    if (out_hw_ok) *out_hw_ok = true;
    return ESP_OK;
}

esp_err_t sx1262_radio_transmit(const uint8_t *data, size_t len,
                                uint32_t bw_hz, uint8_t sf, uint8_t cr,
                                bool cad_enabled,
                                uint32_t *out_air_ms, TaskHandle_t rx_task)
{
    if (!data || len == 0 || len > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lora_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Temporarily mask DIO1 interrupt so background RX task does not clear TxDone/CadDone */
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);

    /* Hardware Channel Activity Detection (Listen-Before-Talk) */
    if (cad_enabled) {
        uint32_t eff_bw = bw_hz ? bw_hz : 250000U;
        uint32_t sym_ms = (uint32_t)((((uint64_t)1000ULL << sf) + eff_bw - 1U) / eff_bw);
        if (sym_ms < 2U) {
            sym_ms = 2U;
        }
        const uint32_t max_cad_wait_ms = sym_ms * 8U + 50U;
        const uint8_t clr_all[2] = { 0x03, 0xFF };
        const uint8_t cad_irq_cfg[8] = { 0x03, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        const uint8_t cad_params[7] = {
            (sf <= 8) ? 0x01 : 0x02,
            (uint8_t)(sf + 13),
            10,
            0x00, /* CAD_ONLY */
            0x00, 0x00, 0x00
        };

        for (int attempt = 0; attempt < 4; attempt++) {
            uint8_t standby_rc = 0x00;
            lora_write_cmd(0x80, &standby_rc, 1);
            lora_write_cmd(0x08, cad_irq_cfg, 8);
            lora_write_cmd(0x02, clr_all, 2);
            lora_write_cmd(0x88, cad_params, 7);
            lora_write_cmd(0xC5, NULL, 0); /* SetCad */

            uint16_t irq = 0;
            uint32_t cad_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            while (((uint32_t)(esp_timer_get_time() / 1000ULL) - cad_start_ms) < max_cad_wait_ms) {
                uint8_t irq_stat[2] = {0};
                if (lora_read_cmd(0x12, irq_stat, 2) == ESP_OK) {
                    irq = ((uint16_t)irq_stat[0] << 8) | irq_stat[1];
                    if (irq & 0x0280) { /* CadDone (0x0080) or Timeout (0x0200) */
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            lora_write_cmd(0x02, clr_all, 2);

            if ((irq & 0x0100) == 0) {
                break; /* Channel is clear */
            }
            ESP_LOGD(TAG, "CAD detected channel activity (attempt %d/4)", attempt + 1);
            if (attempt + 1 < 4) {
                uint8_t rx_cont[3] = { 0xFF, 0xFF, 0xFF };
                lora_write_cmd(0x82, rx_cont, 3);
                uint32_t backoff_ms = (1U + (esp_random() % 3U)) * (sym_ms * 4U + 10U);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            }
        }
    }

    /* Standby */
    uint8_t standby = 0x00;
    lora_write_cmd(0x80, &standby, 1);

    /* Set buffer base address: TX at 0 */
    uint8_t base_addr[2] = { 0x00, 0x00 };
    lora_write_cmd(0x8F, base_addr, 2);

    /* Write data into FIFO */
    esp_err_t err = lora_write_buffer(0x00, data, len);
    if (err != ESP_OK) {
        gpio_intr_enable(BOARD_PIN_LORA_DIO1);
        return err;
    }

    /* Configure PacketParams for TX */
    uint16_t preamble = (sf <= 8) ? 32 : 16;
    uint8_t pkt_params[6] = {
        (uint8_t)(preamble >> 8),
        (uint8_t)(preamble & 0xFF),
        0x00, /* Explicit header */
        (uint8_t)len,
        0x01, /* CRC on */
        0x00  /* Standard IQ */
    };
    lora_write_cmd(0x8C, pkt_params, 6);

    /* Set DIO IRQ: TxDone(0x0001) | Timeout(0x0200) in IrqMask; DIO1 mask 0x0000 */
    uint8_t dio_irq[8] = { 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    lora_write_cmd(0x08, dio_irq, 8);

    /* Clear IRQ */
    uint8_t clr[2] = { 0x03, 0xFF };
    lora_write_cmd(0x02, clr, 2);

    /* Calculate airtime */
    uint32_t air_ms = meshcore_calc_airtime_ms(bw_hz, sf, cr, preamble, len);
    if (out_air_ms) {
        *out_air_ms = air_ms;
    }

    /* Set TX: timeout 0 (disabled in hardware; software polling handles timeout) */
    uint8_t tx_to[3] = { 0x00, 0x00, 0x00 };
    lora_write_cmd(0x83, tx_to, 3);

    /* Poll for TxDone */
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t max_wait_ms = air_ms * 2 + 1000;
    bool tx_ok = false;

    while (((uint32_t)(esp_timer_get_time() / 1000) - start_ms) < max_wait_ms) {
        uint8_t irq_stat[2] = {0};
        if (lora_read_cmd(0x12, irq_stat, 2) == ESP_OK) {
            uint16_t irq = ((uint16_t)irq_stat[0] << 8) | irq_stat[1];
            if (irq & 0x0001) { /* TxDone */
                tx_ok = true;
                break;
            }
            if (irq & 0x0200) { /* Timeout */
                ESP_LOGW(TAG, "TX timeout IRQ triggered");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!tx_ok) {
        ESP_LOGE(TAG, "TX failed or timed out");
    }

    /* Restore continuous RX */
    uint8_t rx_pkt_params[6] = {
        (uint8_t)(preamble >> 8),
        (uint8_t)(preamble & 0xFF),
        0x00, /* Explicit header */
        0xFF, /* Max RX payload */
        0x01, /* CRC on */
        0x00  /* Standard IQ */
    };
    lora_write_cmd(0x8C, rx_pkt_params, 6);

    uint8_t rx_dio_irq[8] = { 0x02, 0x42, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
    lora_write_cmd(0x08, rx_dio_irq, 8);
    lora_write_cmd(0x02, clr, 2);

    /* Enable DIO1 interrupt before SetRx to avoid missing edges */
    gpio_intr_enable(BOARD_PIN_LORA_DIO1);

    uint8_t rx_timeout[3] = { 0xFF, 0xFF, 0xFF };
    lora_write_cmd(0x82, rx_timeout, 3);

    /* If DIO1 latched HIGH immediately, wake the RX task */
    if (gpio_get_level(BOARD_PIN_LORA_DIO1) == 1 && rx_task) {
        xTaskNotifyGive(rx_task);
    }

    return tx_ok ? ESP_OK : ESP_FAIL;
}

bool sx1262_radio_read_inst_rssi(int16_t *out_inst_dbm)
{
    uint8_t rssi_raw = 0;
    if (lora_read_cmd(0x15, &rssi_raw, 1) != ESP_OK) {
        return false;
    }
    if (out_inst_dbm) {
        *out_inst_dbm = -(int16_t)(rssi_raw / 2);
    }
    return true;
}

bool sx1262_radio_poll_rx(uint16_t *out_irq, uint8_t *rx_buf,
                          uint8_t *out_payload_len, int16_t *out_rssi_dbm,
                          int8_t *out_snr_q4, bool *out_read_ok)
{
    uint8_t irq_stat[2] = {0};
    if (lora_read_cmd(0x12, irq_stat, 2) != ESP_OK) {
        return false;
    }

    uint16_t irq = ((uint16_t)irq_stat[0] << 8) | irq_stat[1];
    uint8_t payload_len = 0;
    int16_t rssi = -120;
    int8_t snr_q4 = 0;
    bool read_ok = false;

    if (irq & 0x0002) {
        uint8_t pkt_stat[3] = {0};
        if (lora_read_cmd(0x14, pkt_stat, 3) == ESP_OK) {
            rssi = -(int16_t)(pkt_stat[0] / 2);
            snr_q4 = (int8_t)pkt_stat[1];
        }
        uint8_t rx_buf_stat[2] = {0};
        uint8_t start_offset = 0;
        if (lora_read_cmd(0x13, rx_buf_stat, 2) == ESP_OK) {
            payload_len = rx_buf_stat[0];
            start_offset = rx_buf_stat[1];
        }
        if (rx_buf && payload_len > 0 && lora_read_buffer(start_offset, rx_buf, payload_len) == ESP_OK) {
            read_ok = true;
        }
        uint8_t clr[2] = { 0x03, 0xFF };
        lora_write_cmd(0x02, clr, 2);
    } else if (irq != 0) {
        uint8_t clr[2] = { irq_stat[0], irq_stat[1] };
        lora_write_cmd(0x02, clr, 2);
    }

    if (out_irq)         *out_irq = irq;
    if (out_payload_len) *out_payload_len = payload_len;
    if (out_rssi_dbm)    *out_rssi_dbm = rssi;
    if (out_snr_q4)      *out_snr_q4 = snr_q4;
    if (out_read_ok)     *out_read_ok = read_ok;
    return true;
}
