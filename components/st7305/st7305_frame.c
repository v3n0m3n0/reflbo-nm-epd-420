#include "st7305_frame.h"
#include <stddef.h>

void st7305_frame_to_panel(const uint8_t *canonical, uint8_t *panel)
{
    for (size_t i = 0; i < ST7305_FRAME_BYTES; i++) {
        panel[i] = (uint8_t)~canonical[i];
    }
}

uint8_t ssd1683_encode_temp_reg(int16_t ambient_c)
{
    if (ambient_c >= 15) {
        return 0x64;
    }
    if (ambient_c < -128) {
        ambient_c = -128;
    }
    return (uint8_t)(int8_t)ambient_c;
}
