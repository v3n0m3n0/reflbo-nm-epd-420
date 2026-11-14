#pragma once

/*
 * Hardware abstraction alias header for the software-backed internal ESP32-S3 RTC adapter.
 * Underlying symbols retain the `pcf85063_` prefix for upstream code and host-test compatibility.
 */
#include "pcf85063.h"

#define sw_rtc_init          pcf85063_init
#define sw_rtc_read          pcf85063_read
#define sw_rtc_write         pcf85063_write
#define sw_rtc_set_alarm     pcf85063_set_alarm
#define sw_rtc_clear_alarm   pcf85063_clear_alarm
#define sw_rtc_write_precise pcf85063_write_precise
#define sw_rtc_error_ms      pcf85063_error_ms
#define sw_rtc_set_offset    pcf85063_set_offset
#define sw_rtc_get_offset    pcf85063_get_offset
