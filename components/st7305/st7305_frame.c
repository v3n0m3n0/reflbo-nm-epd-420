#include "st7305_frame.h"
#include <stddef.h>

void st7305_frame_to_panel(const uint8_t *canonical, uint8_t *panel)
{
    for (size_t i = 0; i < ST7305_FRAME_BYTES; i++) {
        panel[i] = (uint8_t)~canonical[i];
    }
}
