#pragma once

/*
 * Hardware abstraction alias header for the RockBase NM-EPD-420 SSD1683 e-paper driver.
 * Underlying symbols retain the `st7305_` prefix for upstream code and test compatibility.
 */
#include "ssd1683_frame.h"
#include "st7305.h"

typedef st7305_variant_t ssd1683_variant_t;

#define SSD1683_VARIANT_FACTORY ST7305_VARIANT_FACTORY
#define SSD1683_VARIANT_XIAOZHI ST7305_VARIANT_XIAOZHI

#define ssd1683_init               st7305_init
#define ssd1683_init_warm          st7305_init_warm
#define ssd1683_prepare_deep_sleep st7305_prepare_deep_sleep
#define ssd1683_reinit             st7305_reinit
#define ssd1683_push               st7305_push
#define ssd1683_seed_previous      st7305_seed_previous
#define ssd1683_partial_count      st7305_partial_count
#define ssd1683_full_count         st7305_full_count
#define ssd1683_busy_timeout_count st7305_busy_timeout_count
#define ssd1683_set_ambient_c      st7305_set_ambient_c
#define ssd1683_cancel_deep_sleep  st7305_cancel_deep_sleep
#define ssd1683_sleep_in           st7305_sleep_in
#define ssd1683_sleep_out          st7305_sleep_out
#define ssd1683_asleep             st7305_asleep
#define ssd1683_variant            st7305_variant
