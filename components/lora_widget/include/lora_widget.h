#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifndef _LORA_CONFIG_T_DEFINED
#define _LORA_CONFIG_T_DEFINED
typedef struct {
    bool     enabled;
    uint32_t freq_khz;     /* e.g. 868000 */
    uint8_t  sf;           /* 7..12 */
    uint16_t bw_khz;       /* 125, 250, 500 */
    uint8_t  cr;           /* 5..8 (4/5..4/8) */
    uint16_t sync_word;    /* e.g. 0x1424 */
    bool     wake_on_rx;
} lora_config_t;
#endif

typedef struct {
    bool     initialized;
    bool     enabled;
    uint32_t freq_khz;
    uint8_t  sf;
    uint16_t bw_khz;
    uint8_t  cr;
    uint16_t sync_word;
    bool     wake_on_rx;
    uint32_t packet_count;
    int16_t  last_rssi_dbm;
    int8_t   last_snr_db;
    uint32_t last_packet_time_s;
    char     last_sender[16];
} lora_stats_t;

/* Starts SX1262 in EU868 RX mode and launches background listener task */
esp_err_t lora_widget_init(void);

/* Applies radio configuration and arms continuous RX mode */
esp_err_t lora_widget_apply(const lora_config_t *cfg);

/* Puts SX1262 into sleep mode and cuts LORA_EN before deep sleep */
void lora_widget_sleep(void);

/* Resets packet counters and last signal metrics */
void lora_widget_reset_stats(void);

/* Wakes the background RX task if a packet is pending */
void lora_widget_notify(void);

/* Returns latest packet reception statistics for UI display */
lora_stats_t lora_widget_get_stats(void);
