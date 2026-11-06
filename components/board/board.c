#include "board.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_TIMEOUT_MS 50 /* at least two 10 ms ticks */

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;

/* Standby writes for ES8311 low-power suspend */
static const uint8_t k_es8311_standby[][2] = {
    { 0x32, 0x00 }, { 0x17, 0x00 }, { 0x0E, 0xFF }, { 0x12, 0x02 }, { 0x14, 0x00 },
    { 0x0D, 0xFA }, { 0x15, 0x00 }, { 0x02, 0x10 }, { 0x00, 0x00 }, { 0x00, 0x1F },
    { 0x01, 0x30 }, { 0x01, 0x00 }, { 0x45, 0x00 }, { 0x0D, 0xFC }, { 0x02, 0x00 },
};

static esp_err_t write_codec(uint16_t addr, const uint8_t (*regs)[2], size_t count)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &cfg, &dev), TAG, "add codec");
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < count && err == ESP_OK; i++) {
        err = i2c_master_transmit(dev, regs[i], 2, I2C_TIMEOUT_MS);
    }
    i2c_master_bus_rm_device(dev);
    return err;
}

static void report_devices(void)
{
    static const struct {
        uint16_t addr;
        const char *name;
    } k_devices[] = {
        { 0x18, "ES8311" },
        { 0x38, "AHT20" },
    };
    for (size_t i = 0; i < sizeof(k_devices) / sizeof(k_devices[0]); i++) {
        esp_err_t err = i2c_master_probe(s_i2c, k_devices[i].addr, I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C 0x%02x %s present", k_devices[i].addr, k_devices[i].name);
        } else {
            ESP_LOGW(TAG, "I2C 0x%02x %s missing: %s", k_devices[i].addr, k_devices[i].name, esp_err_to_name(err));
        }
    }
}

esp_err_t board_init(bool cold)
{
    /*
     * Power rail sequencing for NM-EPD-420:
     * - TEMP_CTL (IO40) and CODEC_EN (IO44) MUST be HIGH during runtime.
     *   Setting either LOW while I2C is active triggers parasitic clamping through ESD diodes to GND.
     * - PA_CTRL (IO41) remains LOW (speaker amplifier muted).
     * - ADC_EN (IO43) remains LOW (battery divider disabled to eliminate drain).
     * - LORA_EN (IO47) set to HIGH (power to SX1262 LoRa module).
     */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_TEMP_CTL) |
                        (1ULL << BOARD_PIN_CODEC_EN) |
                        (1ULL << BOARD_PIN_LORA_EN)  |
                        (1ULL << BOARD_PIN_PA_CTRL)  |
                        (1ULL << BOARD_PIN_ADC_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pwr_cfg), TAG, "power pins config");

    gpio_set_level(BOARD_PIN_TEMP_CTL, 1);
    gpio_set_level(BOARD_PIN_CODEC_EN, 1);
    gpio_set_level(BOARD_PIN_LORA_EN, 1);
    gpio_set_level(BOARD_PIN_PA_CTRL, 0);
    gpio_set_level(BOARD_PIN_ADC_EN, 0);

    /* Allow power rails & sensors to settle */
    vTaskDelay(pdMS_TO_TICKS(cold ? 50 : 10));

    i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "I2C bus");

    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "GPIO ISR service");

    if (cold) {
        report_devices();
    }

    err = write_codec(0x18, k_es8311_standby, sizeof(k_es8311_standby) / sizeof(k_es8311_standby[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 standby failed: %s", esp_err_to_name(err));
    } else if (cold) {
        ESP_LOGI(TAG, "ES8311 audio codec in standby");
    }

    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c(void)
{
    return s_i2c;
}

void board_prepare_deep_sleep(void)
{
    /*
     * Disable pullups on I2C SDA (IO39) and SCL (IO38), as well as I2S lines
     * to ES8311 (IO15, IO16) before pulling TEMP_CTL and CODEC_EN low
     * to prevent parasitic ESD diode leakage to GND.
     */
    gpio_reset_pin(BOARD_PIN_I2C_SDA);
    gpio_reset_pin(BOARD_PIN_I2C_SCL);
    gpio_pullup_dis(BOARD_PIN_I2C_SDA);
    gpio_pullup_dis(BOARD_PIN_I2C_SCL);

    gpio_reset_pin(BOARD_PIN_I2S_SCLK);
    gpio_reset_pin(BOARD_PIN_I2S_DIN);
    gpio_pullup_dis(BOARD_PIN_I2S_SCLK);
    gpio_pullup_dis(BOARD_PIN_I2S_DIN);

    gpio_set_level(BOARD_PIN_TEMP_CTL, 0);
    gpio_set_level(BOARD_PIN_CODEC_EN, 0);
    gpio_set_level(BOARD_PIN_LORA_EN, 0);
    gpio_set_level(BOARD_PIN_PA_CTRL, 0);
    gpio_set_level(BOARD_PIN_ADC_EN, 0);
}
