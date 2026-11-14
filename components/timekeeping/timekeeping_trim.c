#include "timekeeping_trim.h"

#include <stdlib.h>

#define TRIM_VERSION 1

/* a / b to the nearest whole number, halves away from zero; b > 0. */
static int64_t div_round(int64_t a, int64_t b)
{
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

void timekeeping_trim_init(rtc_trim_t *t)
{
    *t = (rtc_trim_t){ .offset = 0, .set_at_ms = 0, .drift_ppb = TRIM_NO_DRIFT };
}

bool timekeeping_trim_measure(rtc_trim_t *t, int64_t now_ms, int64_t rtc_error_ms)
{
    int64_t span = now_ms - t->set_at_ms;
    if (t->set_at_ms == 0 || span < (int64_t)TRIM_MIN_SPAN_S * 1000) {
        return false; /* the correction's two-hour swing would weigh too much */
    }
    const int64_t limit = INT64_MAX / 1000000000; /* keeps error × 10⁹ in range */
    int64_t error = rtc_error_ms > limit ? limit : rtc_error_ms < -limit ? -limit : rtc_error_ms;
    int64_t drift = div_round(error * 1000000000, span);
    drift = drift > INT32_MAX ? INT32_MAX : drift <= INT32_MIN ? INT32_MIN + 1 : drift; /* MIN means none */
    t->drift_ppb = (int32_t)drift;
    if (drift > TRIM_MAX_PPB || drift < -TRIM_MAX_PPB) {
        return false;
    }
    /* The offset in effect already slowed the clock by offset × one step: add that back. */
    int64_t steps = div_round(drift + (int64_t)t->offset * TRIM_STEP_PPB, TRIM_STEP_PPB);
    steps = steps < TRIM_OFFSET_MIN ? TRIM_OFFSET_MIN : steps > TRIM_OFFSET_MAX ? TRIM_OFFSET_MAX : steps;
    int8_t offset = (int8_t)steps;
    bool changed = offset != t->offset;
    t->offset = offset;
    return changed;
}

void timekeeping_trim_set(rtc_trim_t *t, int64_t now_ms)
{
    t->set_at_ms = now_ms;
}

void timekeeping_trim_forget(rtc_trim_t *t)
{
    t->set_at_ms = 0;
}

void timekeeping_trim_pack(const rtc_trim_t *t, uint8_t out[TRIM_RECORD_LEN])
{
    uint64_t at = (uint64_t)t->set_at_ms;
    uint32_t drift = (uint32_t)t->drift_ppb;
    out[0] = TRIM_VERSION;
    out[1] = (uint8_t)t->offset;
    for (int i = 0; i < 8; i++) {
        out[2 + i] = (uint8_t)(at >> (8 * i));
    }
    for (int i = 0; i < 4; i++) {
        out[10 + i] = (uint8_t)(drift >> (8 * i));
    }
}

bool timekeeping_trim_unpack(rtc_trim_t *t, const uint8_t *in, size_t len)
{
    if (in == NULL || len != TRIM_RECORD_LEN || in[0] != TRIM_VERSION) {
        return false;
    }
    int8_t offset = (int8_t)in[1];
    if (offset < TRIM_OFFSET_MIN || offset > TRIM_OFFSET_MAX) {
        return false;
    }
    uint64_t at = 0;
    uint32_t drift = 0;
    for (int i = 0; i < 8; i++) {
        at |= (uint64_t)in[2 + i] << (8 * i);
    }
    for (int i = 0; i < 4; i++) {
        drift |= (uint32_t)in[10 + i] << (8 * i);
    }
    *t = (rtc_trim_t){ .offset = offset, .set_at_ms = (int64_t)at, .drift_ppb = (int32_t)drift };
    return true;
}

int timekeeping_trim_drift_s10_per_day(const rtc_trim_t *t)
{
    if (t->drift_ppb == TRIM_NO_DRIFT) {
        return 0;
    }
    return (int)div_round((int64_t)t->drift_ppb * 864, 1000000); /* ppb × 86 400 s × 10 / 10⁹ */
}

int64_t timekeeping_drift_us(int64_t elapsed_us, int32_t drift_ppb)
{
    if (elapsed_us <= 0 || drift_ppb == TRIM_NO_DRIFT || drift_ppb == 0) {
        return 0;
    }
    int64_t ppb = drift_ppb;
    if (ppb > TIMEKEEPING_SW_DRIFT_MAX_PPB) {
        ppb = TIMEKEEPING_SW_DRIFT_MAX_PPB;
    } else if (ppb < -TIMEKEEPING_SW_DRIFT_MAX_PPB) {
        ppb = -TIMEKEEPING_SW_DRIFT_MAX_PPB;
    }
    int64_t whole = (elapsed_us / 1000000000LL) * ppb;
    int64_t rem = div_round((elapsed_us % 1000000000LL) * ppb, 1000000000LL);
    return whole + rem;
}

uint64_t timekeeping_trim_sleep_us(int64_t left_us, int32_t drift_ppb)
{
    if (left_us <= 1000LL) {
        return 1000ULL;
    }
    int64_t comp_us = timekeeping_drift_us(left_us, drift_ppb);
    int64_t hw_us = left_us + comp_us + 2000LL; /* 2 ms margin so corrected time lands >= target second */
    return hw_us > 1000LL ? (uint64_t)hw_us : 1000ULL;
}

bool timekeeping_sw_drift_measure(int32_t *drift_ppb, int64_t span_ms, int64_t error_ms)
{
    if (drift_ppb == NULL || span_ms < (int64_t)TIMEKEEPING_SW_MIN_SPAN_S * 1000) {
        return false;
    }
    const int64_t limit = INT64_MAX / 1000000000LL;
    if (error_ms > limit || error_ms < -limit) {
        return false;
    }
    int64_t ppb = div_round(error_ms * 1000000000LL, span_ms);
    if (ppb > TIMEKEEPING_SW_DRIFT_MAX_PPB || ppb < -TIMEKEEPING_SW_DRIFT_MAX_PPB) {
        return false;
    }
    *drift_ppb = (int32_t)ppb;
    return true;
}

bool timekeeping_temp_excursion(int16_t cal_temp_c100, int16_t cur_temp_c100, int16_t threshold_c100)
{
    if (cal_temp_c100 == TIMEKEEPING_TEMP_UNSET || cur_temp_c100 == TIMEKEEPING_TEMP_UNSET ||
        threshold_c100 <= 0) {
        return false;
    }
    return abs((int)cur_temp_c100 - (int)cal_temp_c100) >= threshold_c100;
}
