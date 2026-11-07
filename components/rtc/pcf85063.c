#include "pcf85063.h"

#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "rtc";

#define RTC_VALID_MAGIC 0x52544356u /* "RTCV" */
#define RTC_MIN_VALID_EPOCH 1577836800LL /* 2020-01-01 00:00:00 UTC */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR int64_t s_rtc_offset;

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (s_rtc_magic != RTC_VALID_MAGIC) {
        if (tv.tv_sec >= RTC_MIN_VALID_EPOCH) {
            s_rtc_magic = RTC_VALID_MAGIC;
            s_rtc_offset = 0;
        } else {
            s_rtc_offset = 0;
        }
    }
    ESP_LOGI(TAG, "Initialized internal ESP32 RTC (survives deep sleep)");
    return ESP_OK;
}

esp_err_t pcf85063_read(time_t *utc, bool *valid)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (s_rtc_magic == RTC_VALID_MAGIC && (tv.tv_sec + s_rtc_offset < RTC_MIN_VALID_EPOCH)) {
        s_rtc_magic = 0;
        s_rtc_offset = 0;
    }
    if (s_rtc_magic != RTC_VALID_MAGIC && (tv.tv_sec >= RTC_MIN_VALID_EPOCH)) {
        s_rtc_magic = RTC_VALID_MAGIC;
        s_rtc_offset = 0;
    }
    bool v = (s_rtc_magic == RTC_VALID_MAGIC);
    if (valid) {
        *valid = v;
    }
    if (utc) {
        *utc = v ? (time_t)(tv.tv_sec + s_rtc_offset) : 0;
    }
    return ESP_OK;
}

esp_err_t pcf85063_write(time_t utc)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s_rtc_offset = (int64_t)utc - (int64_t)tv.tv_sec;
    s_rtc_magic = RTC_VALID_MAGIC;
    struct timeval new_tv = { .tv_sec = utc, .tv_usec = 0 };
    if (settimeofday(&new_tv, NULL) == 0) {
        s_rtc_offset = 0;
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
    s_rtc_offset = steps;
    return ESP_OK;
}

esp_err_t pcf85063_get_offset(int *steps)
{
    if (steps) {
        *steps = (int)s_rtc_offset;
    }
    return ESP_OK;
}
