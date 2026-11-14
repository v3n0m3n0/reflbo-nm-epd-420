#pragma once

#include <stdint.h>

/* Panel geometry in landscape orientation (spec §4.1). */
#define ST7305_WIDTH       400
#define ST7305_HEIGHT      300
#define ST7305_FRAME_BYTES (ST7305_WIDTH * ST7305_HEIGHT / 8)

/* Converts a canonical frame (row-major 1 bpp, MSB first, 1 = black) into the panel's RAM layout:
 * 2x4-pixel blocks per byte, 1 = white (AGENTS.md gotcha 5). Pure C, host-buildable. */
void st7305_frame_to_panel(const uint8_t *canonical, uint8_t *panel);

/* Encodes ambient temperature (°C) into SSD1683 register 0x1A:
 * >= 15 °C -> 0x64 (OTP warm default); < 15 °C -> clamped [-128, 14] in 8-bit two's complement. */
uint8_t ssd1683_encode_temp_reg(int16_t ambient_c);
#define st7305_encode_temp_reg ssd1683_encode_temp_reg
