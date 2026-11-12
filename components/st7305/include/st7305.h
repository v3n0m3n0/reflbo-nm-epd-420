#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/* Cold start: SPI bus, hardware reset, init sequence. The panel RAM is undefined until the first push. */
esp_err_t st7305_init(st7305_variant_t variant);
/* After a deep-sleep wake: wakes SSD1683 from Deep Sleep Mode 1 (via hw_reset) without clearing bistable e-paper image. */
esp_err_t st7305_init_warm(st7305_variant_t variant, bool asleep);
/* Puts SSD1683 into Deep Sleep Mode 1. Call last, just before sleeping. */
esp_err_t st7305_prepare_deep_sleep(void);
/* Resets the panel and runs another init sequence. */
esp_err_t st7305_reinit(st7305_variant_t variant);
/* Converts and sends a canonical frame (spec §4.1); blocks until BUSY clears or returns ESP_ERR_TIMEOUT. */
esp_err_t st7305_push(const uint8_t *canonical);
/* Seeds previous-panel buffer and partial counter after deep-sleep wake so the next push can use partial refresh. */
esp_err_t st7305_seed_previous(const uint8_t *canonical, size_t len, uint8_t partial_count);
uint8_t st7305_partial_count(void);
uint32_t st7305_full_count(void);
uint32_t st7305_busy_timeout_count(void);
/* Sets ambient temperature in °C for SSD1683 LUT selection (default 25 °C; <15 °C uses measured temp). */
void st7305_set_ambient_c(int16_t temp_c);
/* Undoes st7305_prepare_deep_sleep() after it failed part-way, so the panel can be used again. */
void st7305_cancel_deep_sleep(void);
/* Sleep-in: puts SSD1683 into Deep Sleep Mode 1. */
esp_err_t st7305_sleep_in(void);
/* Wakes the panel from sleep-in with a hardware reset. */
esp_err_t st7305_sleep_out(void);
bool st7305_asleep(void);
st7305_variant_t st7305_variant(void);
