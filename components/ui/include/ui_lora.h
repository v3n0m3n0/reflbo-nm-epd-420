#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "gfx.h"
#include "ui_fields.h"

#define UI_LORA_MAX_MSGS  6
#define UI_LORA_MAX_NODES 3

typedef struct {
    uint32_t timestamp;
    char sender[32];
    char text[128];
    int8_t snr_q4;
    uint8_t hop_count;
    bool is_direct;
    bool outgoing;
    uint8_t channel_idx;
    char channel_name[24];
    uint8_t ack_status;
} ui_lora_msg_t;

typedef struct {
    char name[32];
    uint8_t pub_key_prefix[4];
    uint8_t node_type; /* 1=Chat, 2=Repeater, 3=Room, 4=Sensor */
    int16_t last_rssi_dbm;
    int8_t last_snr_q4;
    uint8_t hops;
    uint32_t last_seen;
} ui_lora_node_t;

struct ui_lora {
    bool enabled;
    bool initialized;
    bool ble_connected;
    uint32_t ble_pin;
    uint32_t freq_khz;
    uint32_t bw_hz;
    uint8_t sf;
    uint8_t cr;
    int8_t tx_power_dbm;
    char node_name[32];
    char channel_name[24];
    uint8_t active_channel_count;
    bool client_repeat;
    uint8_t pub_key_prefix[4];
    uint32_t packet_count;
    uint32_t packets_sent;
    uint32_t sent_flood;
    uint32_t forwarded_count;
    int16_t last_rssi_dbm;
    int8_t last_snr_q4;
    int16_t noise_floor_dbm;
    int msg_count;
    int total_msg_count;
    ui_lora_msg_t msgs[UI_LORA_MAX_MSGS];
    int node_count;
    int total_node_count;
    ui_lora_node_t nodes[UI_LORA_MAX_NODES];
};

void ui_draw_lora_view(gfx_fb_t *fb, gfx_rect_t below, const ui_context_t *ctx);
