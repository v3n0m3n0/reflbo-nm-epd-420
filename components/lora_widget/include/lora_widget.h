#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#endif
#include "meshcore_proto.h"

#ifndef _LORA_CONFIG_T_DEFINED
#define _LORA_CONFIG_T_DEFINED

#define LORA_NODE_NAME_LEN    32
#define LORA_CHANNEL_NAME_LEN 24
#define LORA_CHANNEL_PSK_LEN  48
#define LORA_DEFAULT_PUBLIC_PSK "izOH6cXN6mrJ5e26oRXNcg=="

typedef struct {
    bool enabled;
    uint32_t freq_khz;     /* 430000..928000: kHz (default 868000) */
    uint32_t bw_hz;        /* 7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000 (default 250000) */
    uint16_t bw_khz;       /* integer kHz helper (e.g. 62 for 62.5 kHz, 125, 250, 500) kept in sync with bw_hz */
    uint8_t sf;            /* 5..12 (default 7) */
    uint8_t cr;            /* 5..8 (4/5..4/8, default 5) */
    int8_t tx_power_dbm;   /* -9..22 dBm (default 22) */
    uint16_t sync_word;    /* 0x0000..0xFFFF (default 0x1424) */
    bool rx_boosted;       /* SX1262 RX boosted gain (default true) */
    bool cad_enabled;      /* Channel Activity Detection / LBT (default false) */
    bool client_repeat;    /* Forward flood packets (default false) */
    uint8_t path_hash_mode;/* 0=1B, 1=2B, 2=3B path hashes (default 0) */
    uint8_t airtime_factor_x10; /* 0..90 -> 0.0..9.0 (default 10 = 1.0) */
    bool wake_on_rx;       /* Wake from sleep on DIO1 RX (default false) */
    bool share_loc;        /* Include lat/lon in adverts (default false) */
    uint32_t ble_pin;      /* 0 = auto-generate, or 100000..999999 */
    char node_name[LORA_NODE_NAME_LEN];       /* default "NM-EPD-420" */
    char channel_name[LORA_CHANNEL_NAME_LEN]; /* default "Public" */
    char channel_psk[LORA_CHANNEL_PSK_LEN];   /* default LORA_DEFAULT_PUBLIC_PSK */
} lora_config_t;
#endif

typedef struct {
    bool     initialized;
    bool     enabled;
    uint32_t freq_khz;
    uint32_t bw_hz;
    uint16_t bw_khz;
    uint8_t  sf;
    uint8_t  cr;
    int8_t   tx_power_dbm;
    uint16_t sync_word;
    bool     rx_boosted;
    bool     cad_enabled;
    bool     client_repeat;
    uint8_t  path_hash_mode;
    uint8_t  airtime_factor_x10;
    bool     wake_on_rx;
    bool     share_loc;
    char     node_name[LORA_NODE_NAME_LEN];
    char     channel_name[LORA_CHANNEL_NAME_LEN];
    uint8_t  channel_hash;
    char     pub_key_hex[65];
    int16_t  noise_floor_dbm;
    uint32_t packet_count;
    int16_t  last_rssi_dbm;
    int8_t   last_snr_db;
    int8_t   last_snr_q4; /* SNR * 4 for 0.25 dB resolution */
    uint32_t last_packet_time_s;
    char     last_sender[16];

    /* Extended MeshCore metrics */
    uint32_t packets_sent;
    uint32_t recv_flood;
    uint32_t recv_direct;
    uint32_t sent_flood;
    uint32_t sent_direct;
    uint32_t forwarded_count;
    uint32_t recv_errors;
    uint32_t tx_air_ms;
    uint32_t rx_air_ms;

    /* MeshCore state counts */
    uint8_t advert_count;
    uint8_t neighbor_count;
    uint8_t msg_count;
    uint32_t msg_seq;
    uint32_t advert_seq;

    /* BLE companion state */
    uint32_t ble_pin;
    bool     ble_connected;
    bool     legacy_identity;
} lora_stats_t;

/* Starts SX1262 in EU868 RX mode and launches background listener task */
esp_err_t lora_widget_init(void);

/* Applies radio configuration and arms continuous RX mode */
esp_err_t lora_widget_apply(const lora_config_t *cfg);

/* Updates cached configuration and config-derived stats without powering radio or BLE (used in Focus sleep) */
void lora_widget_sync_config(const lora_config_t *cfg);

/* Puts SX1262 into sleep mode and cuts LORA_EN before deep sleep */
void lora_widget_sleep(void);

/* Resets packet counters and last signal metrics */
void lora_widget_reset_stats(void);

/* Wakes the background RX task if a packet is pending */
void lora_widget_notify(void);

/* Returns latest packet reception statistics for UI display */
lora_stats_t lora_widget_get_stats(void);

/* Sets current device coordinates from main settings for adverts */
void lora_widget_set_location(int32_t lat_e4, int32_t lon_e4);

/* Sets current battery voltage (mV) and percentage (0..100) for Companion App */
void lora_widget_set_battery(uint16_t batt_mv, uint8_t level_pct);

/* Sends a signed MeshCore Node Advert packet (flood or direct) */
esp_err_t lora_widget_send_advert(bool flood);

/* Broadcasts a zero-hop Discover Request packet */
esp_err_t lora_widget_send_discover(void);

/* Sends an encrypted group text message on the configured channel */
esp_err_t lora_widget_send_channel_msg(const char *text);

/* Sends an encrypted group text message with explicit timestamp (or 0 for current time) */
esp_err_t lora_widget_send_channel_msg_ts(uint32_t ts, const char *text);

/* Sends an encrypted group text message on a specific channel index */
esp_err_t lora_widget_send_channel_msg_ex(uint8_t channel_idx, uint32_t ts, const char *text);

/* Sets a callback to be invoked when a new message is received/added or BLE connection changes */
void lora_widget_set_rx_notify_cb(void (*cb)(void));

/* Getters for MeshCore history tables (one item at a time to prevent stack overflow) */
bool lora_widget_get_advert(size_t index, meshcore_advert_t *out);
bool lora_widget_get_neighbor(size_t index, meshcore_neighbor_t *out);
bool lora_widget_get_message(size_t index, meshcore_msg_t *out);

/* Executes a MeshCore CLI command string and formats reply */
void lora_widget_exec_cli(const char *cmd, lora_config_t *cfg_inout, bool *cfg_changed, char *reply, size_t reply_sz);

/* Handles a MeshCore Companion binary command and generates response */
size_t lora_widget_handle_companion_cmd(const uint8_t *cmd, size_t cmd_len,
                                        lora_config_t *cfg_inout, bool *cfg_changed,
                                        uint8_t *resp_out, size_t resp_max);

/* Pops an unread pushed frame for companion app tickle/sync */
bool lora_widget_pop_companion_push(uint8_t *push_out, size_t push_max, size_t *push_len);

/* Returns true if a MeshCore BLE companion client is currently connected */
bool lora_widget_ble_connected(void);

/* Flushes dirty contacts and cleanly stops BLE advertising before entering light sleep */
void lora_widget_prepare_light_sleep(void);

/* Restarts MeshCore BLE advertising if LoRa is enabled */
void lora_widget_resume_ble(void);

/* Sets persistent/NVS-backed node identity seed (or NULL for MAC-derived legacy) and default BLE PIN. */
void lora_widget_set_identity(const uint8_t seed[32], uint32_t default_pin, bool legacy);

/* Registers callback invoked by CLI 'reset-identity' command to rotate NVS identity and apply changes. */
void lora_widget_set_rekey_cb(bool (*cb)(void));

#ifndef _COMPANION_TYPES_DEFINED
#define _COMPANION_TYPES_DEFINED

#define COMPANION_PUB_KEY_SIZE      32
#define COMPANION_MAX_PATH_SIZE     64
#define COMPANION_NAME_SIZE         32
#define COMPANION_MAX_CONTACTS      64
#define COMPANION_MAX_CHANNELS      8

typedef struct {
    uint8_t  pub_key[COMPANION_PUB_KEY_SIZE];
    uint8_t  type;
    uint8_t  flags;
    int8_t   out_path_len;
    uint8_t  out_path[COMPANION_MAX_PATH_SIZE];
    char     name[COMPANION_NAME_SIZE];
    uint32_t last_advert_ts;
    int32_t  lat_e6;
    int32_t  lon_e6;
    uint32_t lastmod;
} companion_contact_t;

typedef struct {
    char    name[COMPANION_NAME_SIZE];
    uint8_t secret[16];
    bool    valid;
} companion_channel_t;

#endif /* _COMPANION_TYPES_DEFINED */

esp_err_t lora_widget_get_channel(uint8_t idx, companion_channel_t *out);
esp_err_t lora_widget_set_channel(uint8_t idx, bool enabled, const char *name, const uint8_t secret[16]);
size_t lora_widget_get_contact_count(void);
esp_err_t lora_widget_get_contact(size_t idx, companion_contact_t *out);
esp_err_t lora_widget_save_contact(const companion_contact_t *contact);
esp_err_t lora_widget_remove_contact(const uint8_t pub_key[32]);
esp_err_t lora_widget_reset_contact_path(const uint8_t pub_key[32]);
esp_err_t lora_widget_send_direct_msg(const uint8_t pub_key[32], const char *text, bool force_flood);
esp_err_t lora_widget_clear_messages(void);
