#pragma once

#include <stdbool.h>
#include <stdint.h>

/* AHT20 data handling: measurement parsing and humidity offset clamping. Pure C, host-buildable. */

/* Parses a 6-byte AHT20 measurement frame (status, hum[20], temp[20]). False on NULL or busy bit. */
bool aht20_parse(const uint8_t raw[6], int *temp_c100, int *hum_pct100);
/* A humidity with its calibration offset, kept within 0-100 %RH (0.01 % units). */
int aht20_offset_humidity(int hum_pct100, int offset_pct100);
