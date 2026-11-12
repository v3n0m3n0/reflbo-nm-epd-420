#include "aht20_codec.h"

#include <stddef.h>

bool aht20_parse(const uint8_t raw[6], int *temp_c100, int *hum_pct100)
{
    if (raw == NULL || temp_c100 == NULL || hum_pct100 == NULL) {
        return false;
    }
    if ((raw[0] & 0x80) != 0) {
        return false;
    }

    uint32_t raw_hum = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | ((uint32_t)raw[3] >> 4);
    uint32_t raw_temp = (((uint32_t)raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | (uint32_t)raw[5];

    *hum_pct100 = (int)(((uint64_t)raw_hum * 10000ULL) >> 20);
    *temp_c100 = (int)((((uint64_t)raw_temp * 20000ULL) >> 20) - 5000);
    return true;
}

int aht20_offset_humidity(int hum_pct100, int offset_pct100)
{
    int h = hum_pct100 + offset_pct100;
    return h < 0 ? 0 : h > 10000 ? 10000 : h;
}
