#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lora_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*------------ MeshCore Companion Command Codes --------------*/
#define CMD_APP_START               1
#define CMD_SEND_TXT_MSG            2
#define CMD_SEND_CHANNEL_TXT_MSG    3
#define CMD_GET_CONTACTS            4
#define CMD_GET_DEVICE_TIME         5
#define CMD_SET_DEVICE_TIME         6
#define CMD_SEND_SELF_ADVERT        7
#define CMD_SET_ADVERT_NAME         8
#define CMD_ADD_UPDATE_CONTACT      9
#define CMD_SYNC_NEXT_MESSAGE       10
#define CMD_SET_RADIO_PARAMS        11
#define CMD_SET_RADIO_TX_POWER      12
#define CMD_RESET_PATH              13
#define CMD_SET_ADVERT_LATLON       14
#define CMD_REMOVE_CONTACT          15
#define CMD_SHARE_CONTACT           16
#define CMD_EXPORT_CONTACT          17
#define CMD_IMPORT_CONTACT          18
#define CMD_REBOOT                  19
#define CMD_GET_BATT_AND_STORAGE    20
#define CMD_SET_TUNING_PARAMS       21
#define CMD_DEVICE_QUERY            22
#define CMD_EXPORT_PRIVATE_KEY      23
#define CMD_IMPORT_PRIVATE_KEY      24
#define CMD_SEND_RAW_DATA           25
#define CMD_SEND_LOGIN              26
#define CMD_SEND_STATUS_REQ         27
#define CMD_HAS_CONNECTION          28
#define CMD_LOGOUT                  29
#define CMD_GET_CONTACT_BY_KEY      30
#define CMD_GET_CHANNEL             31
#define CMD_SET_CHANNEL             32
#define CMD_SIGN_START              33
#define CMD_SIGN_DATA               34
#define CMD_SIGN_FINISH             35
#define CMD_SEND_TRACE_PATH         36
#define CMD_SET_DEVICE_PIN          37
#define CMD_SET_OTHER_PARAMS        38
#define CMD_SEND_TELEMETRY_REQ      39
#define CMD_GET_CUSTOM_VARS         40
#define CMD_SET_CUSTOM_VAR          41
#define CMD_GET_ADVERT_PATH         42
#define CMD_GET_TUNING_PARAMS       43
#define CMD_SEND_BINARY_REQ         50
#define CMD_FACTORY_RESET           51
#define CMD_SEND_PATH_DISCOVERY_REQ 52
#define CMD_SET_FLOOD_SCOPE_KEY     54
#define CMD_SEND_CONTROL_DATA       55
#define CMD_GET_STATS               56
#define CMD_SEND_ANON_REQ           57
#define CMD_SET_AUTOADD_CONFIG      58
#define CMD_GET_AUTOADD_CONFIG      59
#define CMD_GET_ALLOWED_REPEAT_FREQ 60
#define CMD_SET_PATH_HASH_MODE      61
#define CMD_SEND_CHANNEL_DATA       62
#define CMD_SET_DEFAULT_FLOOD_SCOPE 63
#define CMD_GET_DEFAULT_FLOOD_SCOPE 64
#define CMD_SEND_RAW_PACKET         65
#define CMD_RUN_CLI_COMMAND         66

/* Stats sub-types for CMD_GET_STATS */
#define STATS_TYPE_CORE             0
#define STATS_TYPE_RADIO            1
#define STATS_TYPE_PACKETS          2

/*------------ MeshCore Companion Response Codes --------------*/
#define RESP_CODE_OK                  0
#define RESP_CODE_ERR                 1
#define RESP_CODE_CONTACTS_START      2
#define RESP_CODE_CONTACT             3
#define RESP_CODE_END_OF_CONTACTS     4
#define RESP_CODE_SELF_INFO           5
#define RESP_CODE_SENT                6
#define RESP_CODE_CONTACT_MSG_RECV    7
#define RESP_CODE_CHANNEL_MSG_RECV    8
#define RESP_CODE_CURR_TIME           9
#define RESP_CODE_NO_MORE_MESSAGES    10
#define RESP_CODE_EXPORT_CONTACT      11
#define RESP_CODE_BATT_AND_STORAGE    12
#define RESP_CODE_DEVICE_INFO         13
#define RESP_CODE_PRIVATE_KEY         14
#define RESP_CODE_DISABLED            15
#define RESP_CODE_CONTACT_MSG_RECV_V3 16
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
#define RESP_CODE_CHANNEL_INFO        18
#define RESP_CODE_SIGN_START          19
#define RESP_CODE_SIGNATURE           20
#define RESP_CODE_CUSTOM_VARS         21
#define RESP_CODE_ADVERT_PATH         22
#define RESP_CODE_TUNING_PARAMS       23
#define RESP_CODE_STATS               24
#define RESP_CODE_AUTOADD_CONFIG      25
#define RESP_ALLOWED_REPEAT_FREQ      26
#define RESP_CODE_CHANNEL_DATA_RECV   27
#define RESP_CODE_DEFAULT_FLOOD_SCOPE 28
#define RESP_CODE_CLI_REPLY           29

/*------------ Push Codes (Async to app) --------------*/
#define PUSH_CODE_ADVERT                  0x80
#define PUSH_CODE_PATH_UPDATED            0x81
#define PUSH_CODE_SEND_CONFIRMED          0x82
#define PUSH_CODE_MSG_WAITING             0x83
#define PUSH_CODE_RAW_DATA                0x84
#define PUSH_CODE_LOGIN_SUCCESS           0x85
#define PUSH_CODE_LOGIN_FAIL              0x86
#define PUSH_CODE_STATUS_RESPONSE         0x87
#define PUSH_CODE_LOG_RX_DATA             0x88
#define PUSH_CODE_TRACE_DATA              0x89
#define PUSH_CODE_NEW_ADVERT              0x8A
#define PUSH_CODE_TELEMETRY_RESPONSE      0x8B
#define PUSH_CODE_BINARY_RESPONSE         0x8C
#define PUSH_CODE_PATH_DISCOVERY_RESPONSE 0x8D
#define PUSH_CODE_CONTROL_DATA            0x8E
#define PUSH_CODE_CONTACT_DELETED         0x8F
#define PUSH_CODE_CONTACTS_FULL           0x90

/*------------ Error Codes --------------*/
#define COMPANION_ERR_UNSUPPORTED_CMD 1
#define COMPANION_ERR_NOT_FOUND       2
#define COMPANION_ERR_TABLE_FULL      3
#define COMPANION_ERR_BAD_STATE       4
#define COMPANION_ERR_FILE_IO_ERROR   5
#define COMPANION_ERR_ILLEGAL_ARG     6

/*------------ Protocol Limits & Sizes --------------*/
#define COMPANION_FIRMWARE_VER_CODE 10
#define COMPANION_PUB_KEY_SIZE      32
#define COMPANION_MAX_PATH_SIZE     64
#define COMPANION_NAME_SIZE         32
#define COMPANION_MAX_CONTACTS      32
#define COMPANION_MAX_CHANNELS      8
#define COMPANION_MAX_FRAME_SIZE    176
#define COMPANION_CONTACT_FRAME_SIZE 148

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

typedef struct {
    companion_channel_t channels[COMPANION_MAX_CHANNELS];
    companion_contact_t contacts[COMPANION_MAX_CONTACTS];
    size_t              contact_count;

    /* Preferences */
    uint32_t rx_delay_base_x1000;
    uint32_t tx_delay_factor_x1000;
    uint8_t  manual_add_contacts;
    uint8_t  telemetry_mode;
    uint8_t  advert_loc_policy;
    uint8_t  multi_acks;

    /* Outgoing message deduplication state */
    uint32_t last_tx_msg_ts;
    char     last_tx_msg_text[128];
    uint32_t last_tx_seen_s;
} companion_context_t;

typedef struct {
    uint8_t  pub_key[COMPANION_PUB_KEY_SIZE];
    int32_t  lat_e6;
    int32_t  lon_e6;
    uint16_t batt_mv;
    uint32_t storage_used_kb;
    uint32_t storage_total_kb;

    uint32_t (*get_time_s)(void);
    void     (*set_time_s)(uint32_t epoch);

    const lora_stats_t *stats;

    bool (*send_advert)(bool flood);
    bool (*send_channel_txt)(uint8_t channel_idx, const char *text);
    void (*exec_cli)(const char *cmd, char *reply, size_t reply_sz);
    bool (*pop_msg)(uint8_t *msg_out, size_t max, size_t *out_len);
    void (*on_channels_changed)(companion_context_t *ctx, void *user_data);

    void (*send_frame_cb)(const uint8_t *frame, size_t len, void *user_data);
    void *user_data;
} companion_env_t;

void meshcore_companion_init(companion_context_t *ctx);
bool meshcore_companion_add_update_contact(companion_context_t *ctx, const companion_contact_t *contact);
bool meshcore_companion_remove_contact(companion_context_t *ctx, const uint8_t *pub_key);
const companion_contact_t *meshcore_companion_get_contact(const companion_context_t *ctx, const uint8_t *pub_key);

size_t meshcore_companion_handle_cmd(companion_context_t *ctx,
                                     const companion_env_t *env,
                                     lora_config_t *cfg_inout,
                                     bool *cfg_changed,
                                     const uint8_t *cmd,
                                     size_t cmd_len,
                                     uint8_t *resp_out,
                                     size_t resp_max);

#ifdef __cplusplus
}
#endif
