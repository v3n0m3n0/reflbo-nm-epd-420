#include "pcf85063.h"

#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "rtc";

static RTC_DATA_ATTR bool s_rtc_valid;
static RTC_DATA_ATTR int s_rtc_offset;

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    ESP_LOGI(TAG, "Initialized internal ESP32 RTC (survives deep sleep)");
    return ESP_OK;
}

esp_err_t pcf85063_read(time_t *utc, bool *valid)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *utc = tv.tv_sec;
    *valid = s_rtc_valid && (tv.tv_sec >= 1577836800); /* 2020-01-01 */
    return ESP_OK;
}

esp_err_t pcf85063_write(time_t utc)
{
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_rtc_valid = true;
    return ESP_OK;
}

esp_err_t pcf85063_set_alarm(time_t wake)
{
    (void)wake;
    return ESP_OK;
}

esp_err_t pcf85063_clear_alarm(void)
{
    return ESP_OK;
}

esp_err_t pcf85063_write_precise(int64_t *set_at_ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (set_at_ms) {
        *set_at_ms = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
    }
    s_rtc_valid = true;
    return ESP_OK;
}

esp_err_t pcf85063_error_ms(int64_t *error_ms)
{
    if (error_ms) *error_ms = 0;
    return ESP_OK;
}

esp_err_t pcf85063_set_offset(int steps)
{
    s_rtc_offset = steps;
    return ESP_OK;
}

esp_err_t pcf85063_get_offset(int *steps)
{
    if (steps) *steps = s_rtc_offset;
    return ESP_OK;
}
