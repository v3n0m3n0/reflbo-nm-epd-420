#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The RTC trim's arithmetic (spec §7, D25): the PCF85063's Offset register, MODE 0, corrects
 * 4.34 ppm a step, and a positive value slows the clock, so a crystal that runs fast needs a
 * positive offset (datasheet §8.2.3). Each SNTP sync compares the RTC with true time; the drift
 * since the last set to the millisecond, plus what the offset in effect already corrected, is the
 * crystal's own error. Pure C, host-buildable.
 */

#define TRIM_STEP_PPB 4340
#define TRIM_OFFSET_MIN (-64)
#define TRIM_OFFSET_MAX 63
#define TRIM_MIN_SPAN_S (20 * 3600)
#define TRIM_MAX_PPB 270000 /* beyond the register's reach: the clock was set some other way */
#define TRIM_NO_DRIFT INT32_MIN

typedef struct {
    int8_t offset;     /* Offset register steps, in effect since set_at_ms */
    int64_t set_at_ms; /* UTC ms of the last set to the millisecond; 0 = none since a manual set */
    int32_t drift_ppb; /* last measured drift, positive when the RTC ran fast; TRIM_NO_DRIFT if none */
} rtc_trim_t;

void timekeeping_trim_init(rtc_trim_t *t); /* offset 0, nothing measured */
/* At an SNTP sync at true UTC `now_ms` the RTC was `rtc_error_ms` ahead (negative: behind). If it was
 * set to the millisecond at least TRIM_MIN_SPAN_S before, records the drift and picks the offset that
 * cancels the crystal's own error (drift + offset * TRIM_STEP_PPB), rounded and clamped; a drift
 * beyond TRIM_MAX_PPB is recorded but changes nothing. Returns true if the offset changed. */
bool timekeeping_trim_measure(rtc_trim_t *t, int64_t now_ms, int64_t rtc_error_ms);
void timekeeping_trim_set(rtc_trim_t *t, int64_t now_ms); /* the RTC was just set to the millisecond */
void timekeeping_trim_forget(rtc_trim_t *t); /* a manual set: the next sync sets without measuring */
/* The NVS record (sys/rtc_trim): a version byte and the fields, little-endian. */
#define TRIM_RECORD_LEN 14
void timekeeping_trim_pack(const rtc_trim_t *t, uint8_t out[TRIM_RECORD_LEN]);
bool timekeeping_trim_unpack(rtc_trim_t *t, const uint8_t *in, size_t len); /* false: wrong length or version */
/* For display: the drift as seconds a day, tenths (−3.4 s/day → −34); 0 if none. */
int timekeeping_trim_drift_s10_per_day(const rtc_trim_t *t);

/* Software drift compensation for internal 136 kHz RC oscillator (up to ±0.5% = ±5,000,000 ppb = ±432 s/day). */
#define TIMEKEEPING_SW_DRIFT_MAX_PPB 5000000
#define TIMEKEEPING_SW_MIN_SPAN_S 600

/* Signed microsecond drift over `elapsed_us` at `drift_ppb` (positive = clock ran fast).
 * Returns 0 if `elapsed_us <= 0` or `drift_ppb == TRIM_NO_DRIFT`. Overflow-safe 64-bit math. */
int64_t timekeeping_drift_us(int64_t elapsed_us, int32_t drift_ppb);

/* Converts a target real-time sleep duration `left_us` into RTC slow-clock microseconds,
 * compensating for `drift_ppb` and adding a 2 ms wakeup margin past the second boundary. */
uint64_t timekeeping_trim_sleep_us(int64_t left_us, int32_t drift_ppb);

/* Measures software drift in ppb across `span_ms` given `error_ms` (positive = clock ahead).
 * Updates `*drift_ppb` and returns true if `span_ms >= TIMEKEEPING_SW_MIN_SPAN_S * 1000` and
 * the measured drift is within ±TIMEKEEPING_SW_DRIFT_MAX_PPB (jump guard). */
bool timekeeping_sw_drift_measure(int32_t *drift_ppb, int64_t span_ms, int64_t error_ms);

/* Thermal excursion detection for 136 kHz internal RC drift adaptation (centi-degrees Celsius). */
#define TIMEKEEPING_TEMP_UNSET INT16_MIN
#define TIMEKEEPING_THERMAL_DELTA_C100 500 /* 5.00 C */

/* Returns true when neither temperature is TIMEKEEPING_TEMP_UNSET, threshold_c100 > 0,
 * and |cur_temp_c100 - cal_temp_c100| >= threshold_c100. */
bool timekeeping_temp_excursion(int16_t cal_temp_c100, int16_t cur_temp_c100, int16_t threshold_c100);
