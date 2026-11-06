#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "st7305_frame.h"

/*
 * SSD1683 e-paper display driver for NM-EPD-420 (GYE042A87 400x300 monochrome e-paper).
 * Controls SPI display bus (SCK IO2, MOSI IO1, CS IO46, DC IO4, RST IO5, BUSY IO6).
 * Not thread-safe: only one task may call it at a time (display service).
 */

/* The vendor init sequence variants. */
typedef enum {
    ST7305_VARIANT_FACTORY,
    ST7305_VARIANT_XIAOZHI,
} st7305_variant_t;

typedef enum {
    ST7305_MODE_HPM,
    ST7305_MODE_LPM,
} st7305_mode_t;

typedef enum {
    ST7305_LPM_0_25HZ,
    ST7305_LPM_0_5HZ,
    ST7305_LPM_1HZ,
    ST7305_LPM_2HZ,
    ST7305_LPM_4HZ,
    ST7305_LPM_8HZ,
} st7305_lpm_rate_t;

/* Cold start: SPI bus, hardware reset, init sequence. The panel RAM is undefined until the first push. */
esp_err_t st7305_init(st7305_variant_t variant);
/* After a deep-sleep wake: wakes SSD1683 from Deep Sleep Mode 1 (via hw_reset) without clearing bistable e-paper image. */
esp_err_t st7305_init_warm(st7305_variant_t variant, st7305_mode_t mode, st7305_lpm_rate_t rate, bool asleep);
/* Puts SSD1683 into Deep Sleep Mode 1. Call last, just before sleeping. */
esp_err_t st7305_prepare_deep_sleep(void);
/* Resets the panel and runs another init sequence (the SPI clock follows the variant). */
esp_err_t st7305_reinit(st7305_variant_t variant);
/* Converts and sends a canonical frame (spec §4.1); blocks until the DMA transfer has finished. */
esp_err_t st7305_push(const uint8_t *canonical);
/* Switches the power mode with the datasheet §7.11 sequence: about 120 ms into LPM, 320 ms into HPM. */
esp_err_t st7305_set_mode(st7305_mode_t mode);
/* Sets the LPM refresh rate. Before st7305_init it is only stored; every init applies it. */
esp_err_t st7305_set_lpm_rate(st7305_lpm_rate_t rate);
/* Undoes st7305_prepare_deep_sleep() after it failed part-way, so the panel can be used again. */
void st7305_cancel_deep_sleep(void);
/* Sleep-in (datasheet §7.10): from LPM through HPM (300 ms), then SLPIN and 100 ms. The panel stops
 * scanning, so its image fades; RAM and settings are kept. Night sleep (spec §9.1). */
esp_err_t st7305_sleep_in(void);
/* Wakes the panel from sleep-in with a reset and the init sequence, as a plain SLPOUT would reload
 * the NVM defaults (NRDSLP is set). The panel is then in HPM with the RAM cleared: push a frame and
 * set LPM. */
esp_err_t st7305_sleep_out(void);
bool st7305_asleep(void);
st7305_variant_t st7305_variant(void);
st7305_mode_t st7305_mode(void);
st7305_lpm_rate_t st7305_lpm_rate(void);
const char *st7305_lpm_rate_name(st7305_lpm_rate_t rate); /* "0.25" ... "8" (Hz) */
/* Counts the panel's tearing-effect pulses (TE, GPIO6: one per panel frame) for window_ms. */
esp_err_t st7305_count_frames(uint32_t window_ms, uint32_t *frames);
