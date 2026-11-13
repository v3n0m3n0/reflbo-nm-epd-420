#include "timekeeping.h"

#include <stdlib.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "pcf85063.h"
#include "timekeeping_sync.h"

static const char *TAG = "timekeeping";

#define NVS_KEY_TRIM "rtc_trim"
#define NVS_KEY_CAL_TEMP "rtc_cal_t"

static bool s_valid;
static RTC_DATA_ATTR rtc_trim_t s_trim;
static RTC_DATA_ATTR bool s_rtc_trim_loaded;
static bool s_trim_up;
static RTC_DATA_ATTR int64_t s_last_corr_us;
static RTC_DATA_ATTR int64_t s_sw_corr_accum_us;
static RTC_DATA_ATTR int16_t s_cal_temp_c100;
static RTC_DATA_ATTR bool s_cal_temp_set;
static RTC_DATA_ATTR int16_t s_cur_temp_c100;
static RTC_DATA_ATTR bool s_cur_temp_set;

esp_err_t timekeeping_init(const char *tz_posix)
{
    ESP_RETURN_ON_FALSE(setenv("TZ", tz_posix, 1) == 0, ESP_ERR_NO_MEM, TAG, "TZ");
    tzset();
    return ESP_OK;
}

esp_err_t timekeeping_load_from_rtc(bool at_edge)
{
    time_t utc;
    bool valid;
    ESP_RETURN_ON_ERROR(pcf85063_read(&utc, &valid), TAG, "RTC read");
    struct timeval now;
    gettimeofday(&now, NULL);
    if (timekeeping_rtc_resync(now.tv_sec, utc, at_edge)) {
        struct timeval tv = { .tv_sec = utc };
        settimeofday(&tv, NULL);
    }
    s_valid = valid;
    return ESP_OK;
}

bool timekeeping_valid(void)
{
    return s_valid;
}

static void trim_save(void)
{
    uint8_t rec[TRIM_RECORD_LEN];
    timekeeping_trim_pack(&s_trim, rec);
    nvs_handle_t nvs;
    if (nvs_open("sys", NVS_READWRITE, &nvs) == ESP_OK) {
        esp_err_t err = nvs_set_blob(nvs, NVS_KEY_TRIM, rec, sizeof(rec));
        if (s_cal_temp_set && err == ESP_OK) {
            err = nvs_set_i16(nvs, NVS_KEY_CAL_TEMP, s_cal_temp_c100);
        }
        if (err == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}

/* The kept trim, once a boot: a deep-sleep wake keeps the chip's offset but not this copy. A routine
 * wake has no NVS yet (gotcha 11), so it tries again next time rather than take the defaults. */
static void trim_load(void)
{
    if (s_trim_up) {
        return;
    }
    if (!s_rtc_trim_loaded) {
        timekeeping_trim_init(&s_trim);
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("sys", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t rec[TRIM_RECORD_LEN];
        size_t len = sizeof(rec);
        if (nvs_get_blob(nvs, NVS_KEY_TRIM, rec, &len) == ESP_OK && !timekeeping_trim_unpack(&s_trim, rec, len)) {
            timekeeping_trim_init(&s_trim);
        }
        if (s_trim.drift_ppb != TRIM_NO_DRIFT &&
            (s_trim.drift_ppb > TIMEKEEPING_SW_DRIFT_MAX_PPB || s_trim.drift_ppb < -TIMEKEEPING_SW_DRIFT_MAX_PPB)) {
            ESP_LOGW(TAG, "Ignoring out-of-range NVS SW drift (%ld ppb > +/-%ld)",
                     (long)s_trim.drift_ppb, (long)TIMEKEEPING_SW_DRIFT_MAX_PPB);
            s_trim.drift_ppb = TRIM_NO_DRIFT;
            s_cal_temp_set = false;
            s_cal_temp_c100 = 0;
        }
        int16_t cal_t = TIMEKEEPING_TEMP_UNSET;
        if (!s_cal_temp_set && nvs_get_i16(nvs, NVS_KEY_CAL_TEMP, &cal_t) == ESP_OK &&
            cal_t != TIMEKEEPING_TEMP_UNSET) {
            s_cal_temp_c100 = cal_t;
            s_cal_temp_set = true;
        }
        nvs_close(nvs);
    }
    if (err != ESP_ERR_NVS_NOT_INITIALIZED) {
        s_rtc_trim_loaded = true;
        s_trim_up = true;
    }
}

esp_err_t timekeeping_trim_start(void)
{
    trim_load();
    if (s_trim.set_at_ms != 0) {
        timekeeping_trim_forget(&s_trim);
        trim_save();
    }
    ESP_LOGI(TAG, "RTC trim %d steps", s_trim.offset);
    return pcf85063_set_offset(s_trim.offset); /* the chip loses it with its power (D9) */
}

const rtc_trim_t *timekeeping_trim(void)
{
    trim_load();
    return &s_trim;
}

static int64_t clock_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

esp_err_t timekeeping_apply_true_time(int64_t true_utc_us, int64_t mono_us, int64_t *moved_ms)
{
    trim_load();
    int64_t error_ms = 0; /* the RTC against the system clock, while the RTC still keeps its time */
    bool measured = s_valid && pcf85063_error_ms(&error_ms) == ESP_OK;
    int64_t true_now = true_utc_us + (esp_timer_get_time() - mono_us);
    int64_t ahead_ms = (clock_us() - true_now) / 1000; /* the system clock against the truth */
    struct timeval tv = { .tv_sec = (time_t)(true_now / 1000000), .tv_usec = (suseconds_t)(true_now % 1000000) };
    settimeofday(&tv, NULL);
    *moved_ms = -ahead_ms;
    s_trim.offset = 0;
    int64_t total_err_ms = error_ms + ahead_ms + (s_sw_corr_accum_us / 1000LL);
    int64_t span_ms = true_now / 1000 - s_trim.set_at_ms;
    if (measured && s_trim.set_at_ms != 0 &&
        timekeeping_sw_drift_measure(&s_trim.drift_ppb, span_ms, total_err_ms)) {
        ESP_LOGI(TAG, "SW RTC drift updated: %ld ppb", (long)s_trim.drift_ppb);
    } else if (measured && s_trim.set_at_ms != 0 && span_ms >= (int64_t)TIMEKEEPING_SW_MIN_SPAN_S * 1000) {
        ESP_LOGW(TAG, "Rejected SW drift sample (off=%lld ms over %llds); keeping %ld ppb",
                 (long long)total_err_ms, (long long)(span_ms / 1000), (long)s_trim.drift_ppb);
    } else if (measured) {
        ESP_LOGI(TAG, "RTC off by %lld ms (total %lld ms)", (long long)(error_ms + ahead_ms), (long long)total_err_ms);
    }
    int64_t set_at_ms = 0;
    ESP_RETURN_ON_ERROR(pcf85063_write_precise(&set_at_ms), TAG, "RTC write");
    timekeeping_trim_set(&s_trim, set_at_ms);
    s_last_corr_us = true_now;
    s_sw_corr_accum_us = 0;
    s_rtc_trim_loaded = true;
    if (s_cur_temp_set) {
        s_cal_temp_c100 = s_cur_temp_c100;
        s_cal_temp_set = true;
    }
    trim_save();
    s_valid = true;
    return ESP_OK;
}

esp_err_t timekeeping_reset_drift(void)
{
    trim_load();
    timekeeping_trim_init(&s_trim);
    s_cal_temp_c100 = 0;
    s_cal_temp_set = false;
    s_last_corr_us = clock_us();
    s_sw_corr_accum_us = 0;
    s_rtc_trim_loaded = true;
    nvs_handle_t nvs;
    if (nvs_open("sys", NVS_READWRITE, &nvs) == ESP_OK) {
        esp_err_t err = nvs_erase_key(nvs, NVS_KEY_TRIM);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to erase %s: %s", NVS_KEY_TRIM, esp_err_to_name(err));
        }
        err = nvs_erase_key(nvs, NVS_KEY_CAL_TEMP);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to erase %s: %s", NVS_KEY_CAL_TEMP, esp_err_to_name(err));
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ESP_OK;
}

esp_err_t timekeeping_drift_correct(void)
{
    if (!s_valid) {
        return ESP_OK;
    }
    trim_load();
    if (s_trim.drift_ppb == TRIM_NO_DRIFT || s_trim.drift_ppb == 0) {
        return ESP_OK;
    }
    int64_t now_us = clock_us();
    if (s_last_corr_us == 0 || now_us <= s_last_corr_us) {
        s_last_corr_us = now_us;
        return ESP_OK;
    }
    int64_t drift_us = timekeeping_drift_us(now_us - s_last_corr_us, s_trim.drift_ppb);
    if (llabs(drift_us) < 50000LL) {
        return ESP_OK;
    }
    int64_t new_us = now_us - drift_us;
    if (new_us < 0) {
        new_us = 0;
    }
    struct timeval tv = { .tv_sec = (time_t)(new_us / 1000000LL), .tv_usec = (suseconds_t)(new_us % 1000000LL) };
    settimeofday(&tv, NULL);
    s_last_corr_us = new_us;
    s_sw_corr_accum_us += drift_us;
    ESP_LOGI(TAG, "SW drift correction: %lld us", (long long)(-drift_us));
    return ESP_OK;
}

esp_err_t timekeeping_set_utc(time_t utc)
{
    ESP_RETURN_ON_ERROR(pcf85063_write(utc), TAG, "RTC write");
    trim_load(); /* also after a deep-sleep wake: the measurement must not span this set */
    if (s_trim.set_at_ms != 0) {
        timekeeping_trim_forget(&s_trim);
        trim_save();
    }
    struct timeval tv = { .tv_sec = utc };
    settimeofday(&tv, NULL);
    s_last_corr_us = (int64_t)utc * 1000000LL;
    s_sw_corr_accum_us = 0;
    s_valid = true;
    ESP_LOGI(TAG, "time set to %lld", (long long)utc);
    return ESP_OK;
}

void timekeeping_set_ambient_c100(int16_t temp_c100)
{
    s_cur_temp_c100 = temp_c100;
    s_cur_temp_set = true;
}

int16_t timekeeping_cal_temp_c100(void)
{
    trim_load();
    return s_cal_temp_set ? s_cal_temp_c100 : TIMEKEEPING_TEMP_UNSET;
}

int16_t timekeeping_cur_temp_c100(void)
{
    return s_cur_temp_set ? s_cur_temp_c100 : TIMEKEEPING_TEMP_UNSET;
}

bool timekeeping_temp_excursion_active(void)
{
    return timekeeping_temp_excursion(timekeeping_cal_temp_c100(), timekeeping_cur_temp_c100(),
                                      TIMEKEEPING_THERMAL_DELTA_C100);
}
