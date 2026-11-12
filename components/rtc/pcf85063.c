#include "pcf85063.h"

#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "rtc";

#define RTC_VALID_MAGIC 0x52544356u /* "RTCV" */
#define RTC_MIN_VALID_EPOCH 1577836800LL /* 2020-01-01 00:00:00 UTC */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (s_rtc_magic != RTC_VALID_MAGIC && tv.tv_sec >= RTC_MIN_VALID_EPOCH) {
        s_rtc_magic = RTC_VALID_MAGIC;
    }
    ESP_LOGI(TAG, "Initialized internal ESP32 RTC (survives deep sleep)");
    return ESP_OK;
}

esp_err_t pcf85063_read(time_t *utc, bool *valid)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (s_rtc_magic == RTC_VALID_MAGIC && tv.tv_sec < RTC_MIN_VALID_EPOCH) {
        s_rtc_magic = 0;
    } else if (s_rtc_magic != RTC_VALID_MAGIC && tv.tv_sec >= RTC_MIN_VALID_EPOCH) {
        s_rtc_magic = RTC_VALID_MAGIC;
    }
    bool v = (s_rtc_magic == RTC_VALID_MAGIC);
    if (valid) {
        *valid = v;
    }
    if (utc) {
        *utc = v ? tv.tv_sec : 0;
    }
    return ESP_OK;
}

esp_err_t pcf85063_write(time_t utc)
{
    struct timeval new_tv = { .tv_sec = utc, .tv_usec = 0 };
    if (settimeofday(&new_tv, NULL) == 0) {
        s_rtc_magic = (utc >= RTC_MIN_VALID_EPOCH) ? RTC_VALID_MAGIC : 0;
    }
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
    s_rtc_magic = RTC_VALID_MAGIC;
    return ESP_OK;
}

esp_err_t pcf85063_error_ms(int64_t *error_ms)
{
    if (error_ms) *error_ms = 0;
    return ESP_OK;
}

esp_err_t pcf85063_set_offset(int steps)
{
    (void)steps; /* internal RTC has no trim register; drift is compensated in timekeeping */
    return ESP_OK;
}

esp_err_t pcf85063_get_offset(int *steps)
{
    if (steps) {
        *steps = 0;
    }
    return ESP_OK;
}
