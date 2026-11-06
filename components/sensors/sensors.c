#include "sensors.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "shtc3_codec.h"
#include "util_ticks.h"

/* AHT20 on RockBase NM-EPD-420 */
#define AHT20_ADDR          0x38
#define AHT20_CMD_STATUS    0x71
#define AHT20_CMD_INIT      0xBE  /* AHT20 strictly requires 0xBE, 0x08, 0x00 */
#define AHT20_CMD_MEASURE   0xAC
#define AHT20_INIT_DELAY_MS 80
#define AHT20_MEAS_DELAY_MS 55
#define AHT20_MEAS_STEP_MS  10
#define AHT20_MEAS_RETRIES  3
#define I2C_TIMEOUT_MS      50    /* >= two 10 ms ticks */

#define LEARN_MIN_EPOCH     1577836800 /* 2020-01-01 */
#define BAT_CHANNEL         ADC_CHANNEL_2 /* GPIO3 = ADC1_CH2 */
#define BAT_SAMPLES         16
#define BAT_DIVIDER         2             /* 100k / 100k (ratio 2) */

static const char *TAG = "sensors";

static i2c_master_dev_handle_t s_aht20;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static sensors_state_t s_state;
static int s_temp_offset_c100 = CONFIG_REFLBO_TEMP_OFFSET_C10 * 10;
static int s_hum_offset_pct100 = CONFIG_REFLBO_HUM_OFFSET_PCT10 * 10;

static esp_err_t aht20_check_and_init(void)
{
    uint8_t cmd = AHT20_CMD_STATUS;
    uint8_t status = 0;
    esp_err_t err = i2c_master_transmit_receive(s_aht20, &cmd, 1, &status, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        err = i2c_master_receive(s_aht20, &status, 1, I2C_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 status query failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Bit 3 of status indicates calibrated state */
    if ((status & 0x08) == 0) {
        ESP_LOGI(TAG, "AHT20 uncalibrated (status 0x%02X), sending init 0xBE...", status);
        const uint8_t init_cmd[3] = { AHT20_CMD_INIT, 0x08, 0x00 };
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_aht20, init_cmd, sizeof(init_cmd), I2C_TIMEOUT_MS),
                            TAG, "aht20 init cmd");
        vTaskDelay(pdMS_TO_TICKS(AHT20_INIT_DELAY_MS));
    }
    return ESP_OK;
}

static esp_err_t aht20_read(int *temp_c100, int *hum_pct100)
{
    const uint8_t meas_cmd[3] = { AHT20_CMD_MEASURE, 0x33, 0x00 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_aht20, meas_cmd, sizeof(meas_cmd), I2C_TIMEOUT_MS),
                        TAG, "trigger measurement");

    vTaskDelay(pdMS_TO_TICKS(AHT20_MEAS_DELAY_MS));

    uint8_t status = 0x80;
    esp_err_t err = ESP_OK;
    for (int retry = 0; retry <= AHT20_MEAS_RETRIES; retry++) {
        err = i2c_master_receive(s_aht20, &status, 1, I2C_TIMEOUT_MS);
        if (err == ESP_OK && (status & 0x80) == 0) {
            break;
        }
        if (retry < AHT20_MEAS_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(AHT20_MEAS_STEP_MS));
        }
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read status");

    if (status & 0x80) {
        ESP_LOGW(TAG, "AHT20 still busy after measurement wait");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[6] = {0};
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_aht20, raw, sizeof(raw), I2C_TIMEOUT_MS),
                        TAG, "read measurement");

    if (raw[0] & 0x80) {
        ESP_LOGW(TAG, "AHT20 busy during measurement read");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t raw_hum = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | ((uint32_t)raw[3] >> 4);
    uint32_t raw_temp = (((uint32_t)raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | (uint32_t)raw[5];

    *hum_pct100 = (int)((raw_hum * 10000ULL) >> 20);
    *temp_c100 = (int)(((raw_temp * 20000ULL) >> 20) - 5000);

    return ESP_OK;
}

static esp_err_t adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_unit_handle_t adc = NULL;
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &adc), TAG, "ADC unit");

    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    esp_err_t err = adc_oneshot_config_channel(adc, BAT_CHANNEL, &chan);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(adc);
        ESP_LOGE(TAG, "ADC channel config: %s", esp_err_to_name(err));
        return err;
    }
    s_adc = adc;

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = BAT_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC curve fitting in eFuse; using line fitting or approximate voltage");
        s_cali = NULL;
    }
    return ESP_OK;
}

esp_err_t sensors_init(i2c_master_bus_handle_t bus, bool cold)
{
    if (s_aht20 == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AHT20_ADDR,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_aht20), TAG, "add AHT20");
    }
    if (cold) {
        battery_gauge_init(&s_state.gauge);
        esp_err_t err = aht20_check_and_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 not ready: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t sensors_sample_env(time_t now)
{
    int t = 0, h = 0;
    esp_err_t err = aht20_read(&t, &h);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = aht20_read(&t, &h); /* Retry once */
    }
    ESP_RETURN_ON_ERROR(err, TAG, "AHT20 read error");

    s_state.env = (sensors_env_t){
        .valid = true,
        .temp_c100 = t + s_temp_offset_c100,
        .hum_pct100 = shtc3_offset_humidity(h, s_hum_offset_pct100),
        .time = now,
    };
    return ESP_OK;
}

esp_err_t sensors_sample_battery(time_t now)
{
    if (s_adc == NULL) {
        ESP_RETURN_ON_ERROR(adc_init(), TAG, "ADC init");
    }

    /*
     * Battery ADC protocol for NM-EPD-420:
     * 1. Drive PIN_ADC_EN (IO43) HIGH.
     * 2. Wait 20 ms for decoupling caps to settle.
     * 3. Read samples.
     * 4. Drive PIN_ADC_EN LOW to prevent battery drain.
     */
    gpio_set_level(BOARD_PIN_ADC_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    int raw = 0, sum = 0, count = 0;
    adc_oneshot_read(s_adc, BAT_CHANNEL, &raw); /* Discard first read */
    for (int i = 0; i < BAT_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc, BAT_CHANNEL, &raw) != ESP_OK) {
            continue;
        }
        int mv = raw * 3100 / 4095;
        if (s_cali != NULL) {
            adc_cali_raw_to_voltage(s_cali, raw, &mv);
        }
        sum += mv;
        count++;
    }

    gpio_set_level(BOARD_PIN_ADC_EN, 0);

    ESP_RETURN_ON_FALSE(count > 0, ESP_FAIL, TAG, "ADC read failed");
    int mv = sum / count * BAT_DIVIDER * CONFIG_REFLBO_BATTERY_FACTOR_PERMILLE / 1000;
    s_state.last_mv = mv;
    s_state.battery_time = now;
    battery_gauge_add(&s_state.gauge, (uint32_t)now, mv);

    if (now >= LEARN_MIN_EPOCH) {
        battery_learn_t *l = &s_state.learn;
        int points = battery_learn_points(l);
        uint8_t state = l->state;
        battery_learn_add(l, (uint32_t)now, battery_gauge_mv(&s_state.gauge),
                          battery_gauge_state(&s_state.gauge, (uint32_t)now));
        s_state.learn_changed |= battery_learn_points(l) != points || l->state != state;
    }
    return ESP_OK;
}

void sensors_learn_start(void)
{
    battery_learn_start(&s_state.learn);
    s_state.learn_changed = true;
}

void sensors_learn_stop(void)
{
    battery_learn_stop(&s_state.learn);
    s_state.learn_changed = true;
}

const battery_learn_t *sensors_learn(void)
{
    return &s_state.learn;
}

bool sensors_learn_take_curve(uint16_t curve[BATTERY_CURVE_POINTS])
{
    if (!battery_learn_curve(&s_state.learn, curve)) {
        return false;
    }
    battery_learn_stop(&s_state.learn);
    s_state.learn_changed = true;
    return true;
}

bool sensors_learn_take_changed(void)
{
    bool changed = s_state.learn_changed;
    s_state.learn_changed = false;
    return changed;
}

void sensors_learn_restore(const battery_learn_t *l)
{
    s_state.learn = *l;
}

void sensors_set_offsets(int temp_c100, int hum_pct100)
{
    s_temp_offset_c100 = temp_c100;
    s_hum_offset_pct100 = hum_pct100;
}

void sensors_set_battery_cal(const battery_cal_t *cal)
{
    if (memcmp(cal, &s_state.gauge.cal, sizeof(*cal)) != 0) {
        battery_gauge_set_cal(&s_state.gauge, cal);
    }
}

int sensors_battery_days_left10(time_t now)
{
    return battery_gauge_days_left10(&s_state.gauge, (uint32_t)now);
}

void sensors_shift_time(int64_t delta_s)
{
    battery_gauge_shift_time(&s_state.gauge, delta_s);
    battery_learn_shift_time(&s_state.learn, delta_s);
    if (s_state.env.time != 0) {
        s_state.env.time += (time_t)delta_s;
    }
    if (s_state.battery_time != 0) {
        s_state.battery_time += (time_t)delta_s;
    }
}

sensors_env_t sensors_env(void)
{
    return s_state.env;
}

sensors_battery_t sensors_battery(time_t now)
{
    return (sensors_battery_t){
        .valid = battery_gauge_level(&s_state.gauge) >= 0,
        .last_mv = s_state.last_mv,
        .smoothed_mv = battery_gauge_mv(&s_state.gauge),
        .level = battery_gauge_level(&s_state.gauge),
        .state = battery_gauge_state(&s_state.gauge, (uint32_t)now),
        .time = s_state.battery_time,
    };
}

void sensors_export(sensors_state_t *out)
{
    *out = s_state;
}

void sensors_import(const sensors_state_t *in)
{
    s_state = *in;
}
