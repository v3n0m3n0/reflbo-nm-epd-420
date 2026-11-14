#pragma once

/*
 * Hardware abstraction alias header for the RockBase NM-EPD-420 SSD1683 e-paper frame helpers.
 * Underlying symbol names keep the `st7305_` prefix for upstream host-test compatibility.
 */
#include "st7305_frame.h"

#define SSD1683_WIDTH       ST7305_WIDTH
#define SSD1683_HEIGHT      ST7305_HEIGHT
#define SSD1683_FRAME_BYTES ST7305_FRAME_BYTES

#define ssd1683_frame_to_panel st7305_frame_to_panel
