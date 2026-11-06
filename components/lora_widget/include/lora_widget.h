#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t packet_count;
    int16_t  last_rssi_dbm;
    int8_t   last_snr_db;
    uint32_t last_packet_time_s;
    char     last_sender[16];
} lora_stats_t;

/* Starts SX1262 in EU868 RX mode and launches background listener task */
esp_err_t lora_widget_init(void);

/* Returns latest packet reception statistics for UI display */
lora_stats_t lora_widget_get_stats(void);
