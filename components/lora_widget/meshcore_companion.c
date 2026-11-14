#include "meshcore_companion.h"
#include <string.h>

static const uint8_t k_public_psk[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};

static inline void wr_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void wr_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint16_t rd_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static inline uint32_t rd_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static inline size_t write_ok(uint8_t *resp, size_t max)
{
    if (max < 1) return 0;
    resp[0] = RESP_CODE_OK;
    return 1;
}

static inline size_t write_err(uint8_t *resp, size_t max, uint8_t err_code)
{
    if (max < 2) return 0;
    resp[0] = RESP_CODE_ERR;
    resp[1] = err_code;
    return 2;
}

static inline void copy_cstr(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    size_t n = 0;
    if (src) {
        while (n + 1 < dst_sz && src[n] != '\0') {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
}

static inline size_t write_disabled(uint8_t *resp, size_t max)
{
    if (max < 1) return 0;
    resp[0] = RESP_CODE_DISABLED;
    return 1;
}

void meshcore_companion_init(companion_context_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));

    /* Slot 0 is initialized as Public channel */
    ctx->channels[0].valid = true;
    copy_cstr(ctx->channels[0].name, sizeof(ctx->channels[0].name), "Public");
    memcpy(ctx->channels[0].secret, k_public_psk, 16);

    ctx->rx_delay_base_x1000 = 10000;
    ctx->tx_delay_factor_x1000 = 500;
    ctx->autoadd_config = 0x1E;
    ctx->autoadd_max_hops = 64;
}

bool meshcore_companion_add_update_contact(companion_context_t *ctx, const companion_contact_t *contact)
{
    if (!ctx || !contact) return false;

    for (size_t i = 0; i < ctx->contact_count; i++) {
        if (memcmp(ctx->contacts[i].pub_key, contact->pub_key, COMPANION_PUB_KEY_SIZE) == 0) {
            ctx->contacts[i] = *contact;
            return true;
        }
    }

    if (ctx->contact_count < COMPANION_MAX_CONTACTS) {
        ctx->contacts[ctx->contact_count++] = *contact;
        return true;
    }

    return false;
}

bool meshcore_companion_remove_contact(companion_context_t *ctx, const uint8_t *pub_key)
{
    if (!ctx || !pub_key) return false;

    for (size_t i = 0; i < ctx->contact_count; i++) {
        if (memcmp(ctx->contacts[i].pub_key, pub_key, COMPANION_PUB_KEY_SIZE) == 0) {
            for (size_t j = i; j + 1 < ctx->contact_count; j++) {
                ctx->contacts[j] = ctx->contacts[j + 1];
            }
            ctx->contact_count--;
            return true;
        }
    }
    return false;
}

const companion_contact_t *meshcore_companion_get_contact(const companion_context_t *ctx, const uint8_t *pub_key)
{
    if (!ctx || !pub_key) return NULL;

    for (size_t i = 0; i < ctx->contact_count; i++) {
        if (memcmp(ctx->contacts[i].pub_key, pub_key, COMPANION_PUB_KEY_SIZE) == 0) {
            return &ctx->contacts[i];
        }
    }
    return NULL;
}

const companion_contact_t *meshcore_companion_find_contact_by_prefix(const companion_context_t *ctx,
                                                                     const uint8_t prefix[6])
{
    if (!ctx || !prefix) return NULL;

    for (size_t i = 0; i < ctx->contact_count; i++) {
        if (memcmp(ctx->contacts[i].pub_key, prefix, 6) == 0) {
            return &ctx->contacts[i];
        }
    }
    return NULL;
}

companion_contact_t *meshcore_companion_get_contact_mut(companion_context_t *ctx, const uint8_t *pub_key)
{
    if (!ctx || !pub_key) return NULL;

    for (size_t i = 0; i < ctx->contact_count; i++) {
        if (memcmp(ctx->contacts[i].pub_key, pub_key, COMPANION_PUB_KEY_SIZE) == 0) {
            return &ctx->contacts[i];
        }
    }
    return NULL;
}

static size_t format_contact_frame(const companion_contact_t *c, uint8_t *out, size_t max)
{
    if (max < COMPANION_CONTACT_FRAME_SIZE) return 0;
    size_t i = 0;
    out[i++] = RESP_CODE_CONTACT;
    memcpy(&out[i], c->pub_key, COMPANION_PUB_KEY_SIZE);
    i += COMPANION_PUB_KEY_SIZE;
    out[i++] = c->type;
    out[i++] = c->flags;
    out[i++] = (uint8_t)c->out_path_len;
    memcpy(&out[i], c->out_path, COMPANION_MAX_PATH_SIZE);
    i += COMPANION_MAX_PATH_SIZE;
    memset(&out[i], 0, COMPANION_NAME_SIZE);
    copy_cstr((char *)&out[i], COMPANION_NAME_SIZE, c->name);
    i += COMPANION_NAME_SIZE;
    wr_le32(&out[i], c->last_advert_ts);
    i += 4;
    wr_le32(&out[i], (uint32_t)c->lat_e6);
    i += 4;
    wr_le32(&out[i], (uint32_t)c->lon_e6);
    i += 4;
    wr_le32(&out[i], c->lastmod);
    i += 4;
    return i;
}

bool meshcore_companion_step_iter(companion_context_t *ctx, const companion_env_t *env)
{
    if (!ctx || !ctx->contact_iter_active || !env || !env->send_frame_cb) {
        return false;
    }

    while (ctx->contact_iter_idx < ctx->contact_count) {
        size_t i = ctx->contact_iter_idx++;
        if (ctx->contacts[i].lastmod > ctx->contact_iter_since) {
            if (ctx->contacts[i].lastmod > ctx->contact_iter_lastmod) {
                ctx->contact_iter_lastmod = ctx->contacts[i].lastmod;
            }
            uint8_t c_frame[COMPANION_CONTACT_FRAME_SIZE];
            size_t c_len = format_contact_frame(&ctx->contacts[i], c_frame, sizeof(c_frame));
            if (c_len > 0) {
                env->send_frame_cb(c_frame, c_len, env->user_data);
            }
            return true; /* One contact sent; iterator remains active */
        }
    }

    /* All matching contacts sent; send end frame */
    uint8_t end_frame[5];
    end_frame[0] = RESP_CODE_END_OF_CONTACTS;
    wr_le32(&end_frame[1], ctx->contact_iter_lastmod);
    env->send_frame_cb(end_frame, 5, env->user_data);
    ctx->contact_iter_active = false;
    return false; /* Iteration complete */
}

size_t meshcore_companion_handle_cmd(companion_context_t *ctx,
                                     const companion_env_t *env,
                                     lora_config_t *cfg,
                                     bool *cfg_changed,
                                     const uint8_t *cmd,
                                     size_t cmd_len,
                                     uint8_t *resp_out,
                                     size_t resp_max)
{
    if (!ctx || !env || !cfg || !cmd || cmd_len == 0 || !resp_out || resp_max == 0) {
        return 0;
    }

    /* Any incoming command interrupts an active contacts iterator (MyMesh.cpp:1063) */
    ctx->contact_iter_active = false;

    uint8_t opcode = cmd[0];

    switch (opcode) {
    case CMD_APP_START: {
        if (resp_max < 58) return 0;
        size_t i = 0;
        resp_out[i++] = RESP_CODE_SELF_INFO;
        resp_out[i++] = 1; /* ADV_TYPE_CHAT */
        resp_out[i++] = (uint8_t)cfg->tx_power_dbm;
        resp_out[i++] = 22; /* MAX_LORA_TX_POWER */
        memcpy(&resp_out[i], env->pub_key, COMPANION_PUB_KEY_SIZE);
        i += COMPANION_PUB_KEY_SIZE;
        int32_t lat_e6 = ctx->has_latlon ? ctx->lat_e6 : env->lat_e6;
        int32_t lon_e6 = ctx->has_latlon ? ctx->lon_e6 : env->lon_e6;
        wr_le32(&resp_out[i], (uint32_t)lat_e6);
        i += 4;
        wr_le32(&resp_out[i], (uint32_t)lon_e6);
        i += 4;
        resp_out[i++] = ctx->multi_acks;
        resp_out[i++] = ctx->advert_loc_policy;
        resp_out[i++] = ctx->telemetry_mode;
        resp_out[i++] = ctx->manual_add_contacts;
        wr_le32(&resp_out[i], cfg->freq_khz);
        i += 4;
        wr_le32(&resp_out[i], cfg->bw_hz);
        i += 4;
        resp_out[i++] = cfg->sf;
        resp_out[i++] = cfg->cr;
        size_t nlen = strlen(cfg->node_name);
        if (i + nlen > resp_max) nlen = resp_max - i;
        memcpy(&resp_out[i], cfg->node_name, nlen);
        i += nlen;
        return i;
    }

    case CMD_DEVICE_QUERY: {
        if (resp_max < 82) return 0;
        size_t i = 0;
        resp_out[i++] = RESP_CODE_DEVICE_INFO;
        resp_out[i++] = COMPANION_FIRMWARE_VER_CODE;
        resp_out[i++] = COMPANION_MAX_CONTACTS / 2;
        resp_out[i++] = COMPANION_MAX_CHANNELS;
        wr_le32(&resp_out[i], cfg->ble_pin);
        i += 4;
        memset(&resp_out[i], 0, 12);
        {
            const char *bdate = __DATE__;
            size_t dlen = strlen(bdate);
            if (dlen > 11) dlen = 11;
            memcpy(&resp_out[i], bdate, dlen);
        }
        i += 12;
        memset(&resp_out[i], 0, 40);
        copy_cstr((char *)&resp_out[i], 40, "RockBase NM-EPD-420");
        i += 40;
        memset(&resp_out[i], 0, 20);
        copy_cstr((char *)&resp_out[i], 20, "v1.18.0-reflbo-1.1");
        i += 20;
        resp_out[i++] = cfg->client_repeat ? 1 : 0;
        resp_out[i++] = cfg->path_hash_mode;
        return i;
    }

    case CMD_GET_CHANNEL: {
        if (cmd_len < 2) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        uint8_t idx = cmd[1];
        if (idx >= COMPANION_MAX_CHANNELS) {
            return write_err(resp_out, resp_max, COMPANION_ERR_NOT_FOUND);
        }
        if (resp_max < 50) return 0;
        size_t i = 0;
        resp_out[i++] = RESP_CODE_CHANNEL_INFO;
        resp_out[i++] = idx;
        memset(&resp_out[i], 0, 32);
        if (ctx->channels[idx].valid) {
            copy_cstr((char *)&resp_out[i], 32, ctx->channels[idx].name);
        }
        i += 32;
        memset(&resp_out[i], 0, 16);
        if (ctx->channels[idx].valid) {
            memcpy(&resp_out[i], ctx->channels[idx].secret, 16);
        }
        i += 16;
        return i;
    }

    case CMD_SET_CHANNEL: {
        if (cmd_len < 2 + 32 + 16) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        uint8_t idx = cmd[1];
        if (idx >= COMPANION_MAX_CHANNELS) return write_err(resp_out, resp_max, COMPANION_ERR_NOT_FOUND);
        memset(ctx->channels[idx].name, 0, sizeof(ctx->channels[idx].name));
        memcpy(ctx->channels[idx].name, &cmd[2], 32);
        ctx->channels[idx].name[31] = '\0';
        memcpy(ctx->channels[idx].secret, &cmd[34], 16);

        /* A channel with empty name and all-zero secret is deleted/invalid */
        bool all_zero_secret = true;
        for (int s = 0; s < 16; s++) {
            if (ctx->channels[idx].secret[s] != 0) {
                all_zero_secret = false;
                break;
            }
        }
        if (ctx->channels[idx].name[0] == '\0' && all_zero_secret) {
            ctx->channels[idx].valid = false;
        } else {
            ctx->channels[idx].valid = true;
        }

        if (env && env->on_channels_changed) {
            env->on_channels_changed(ctx, env->user_data);
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_GET_STATS: {
        uint8_t stype = (cmd_len >= 2) ? cmd[1] : 0;
        if (stype == STATS_TYPE_CORE) {
            if (resp_max < 11) return 0;
            resp_out[0] = RESP_CODE_STATS;
            resp_out[1] = STATS_TYPE_CORE;
            wr_le16(&resp_out[2], env->batt_mv);
            uint32_t uptime_s = env->get_time_s ? env->get_time_s() : 0;
            wr_le32(&resp_out[4], uptime_s);
            wr_le16(&resp_out[8], 0);
            resp_out[10] = 0;
            return 11;
        } else if (stype == STATS_TYPE_RADIO) {
            if (resp_max < 14) return 0;
            resp_out[0] = RESP_CODE_STATS;
            resp_out[1] = STATS_TYPE_RADIO;
            int16_t nf = env->stats ? env->stats->noise_floor_dbm : -120;
            int8_t last_rssi = env->stats ? (int8_t)env->stats->last_rssi_dbm : -120;
            int8_t last_snr_x4 = env->stats ? env->stats->last_snr_q4 : 0;
            uint32_t tx_air_s = env->stats ? (env->stats->tx_air_ms / 1000) : 0;
            uint32_t rx_air_s = env->stats ? (env->stats->rx_air_ms / 1000) : 0;
            wr_le16(&resp_out[2], (uint16_t)nf);
            resp_out[4] = (uint8_t)last_rssi;
            resp_out[5] = (uint8_t)last_snr_x4;
            wr_le32(&resp_out[6], tx_air_s);
            wr_le32(&resp_out[10], rx_air_s);
            return 14;
        } else if (stype == STATS_TYPE_PACKETS) {
            if (resp_max < 30) return 0;
            resp_out[0] = RESP_CODE_STATS;
            resp_out[1] = STATS_TYPE_PACKETS;
            wr_le32(&resp_out[2], env->stats ? env->stats->packet_count : 0);
            wr_le32(&resp_out[6], env->stats ? env->stats->packets_sent : 0);
            wr_le32(&resp_out[10], env->stats ? env->stats->sent_flood : 0);
            wr_le32(&resp_out[14], env->stats ? env->stats->sent_direct : 0);
            wr_le32(&resp_out[18], env->stats ? env->stats->recv_flood : 0);
            wr_le32(&resp_out[22], env->stats ? env->stats->recv_direct : 0);
            wr_le32(&resp_out[26], env->stats ? env->stats->recv_errors : 0);
            return 30;
        }
        return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
    }

    case CMD_SET_RADIO_PARAMS: {
        if (cmd_len < 11) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        uint32_t freq_khz = rd_le32(&cmd[1]);
        uint32_t bw_hz = rd_le32(&cmd[5]);
        uint8_t sf = cmd[9];
        uint8_t cr = cmd[10];

        if (freq_khz < 150000 || freq_khz > 2500000 || sf < 5 || sf > 12 ||
            cr < 5 || cr > 8 || bw_hz < 7000 || bw_hz > 500000) {
            return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        }

        cfg->freq_khz = freq_khz;
        cfg->bw_hz = bw_hz;
        cfg->bw_khz = (uint16_t)(bw_hz / 1000);
        cfg->sf = sf;
        cfg->cr = cr;
        if (cmd_len > 11) {
            cfg->client_repeat = (cmd[11] != 0);
        }
        if (cfg_changed) *cfg_changed = true;
        return write_ok(resp_out, resp_max);
    }

    case CMD_SET_RADIO_TX_POWER: {
        if (cmd_len < 2) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        int8_t pwr = (int8_t)cmd[1];
        if (pwr < -9 || pwr > 22) {
            return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        }
        cfg->tx_power_dbm = pwr;
        if (cfg_changed) *cfg_changed = true;
        return write_ok(resp_out, resp_max);
    }

    case CMD_SET_DEVICE_PIN: {
        if (cmd_len < 5) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        uint32_t pin = rd_le32(&cmd[1]);
        if (pin == 0 || (pin >= 100000 && pin <= 999999)) {
            cfg->ble_pin = pin;
            if (cfg_changed) *cfg_changed = true;
            return write_ok(resp_out, resp_max);
        }
        return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
    }

    case CMD_GET_CONTACTS: {
        uint32_t since = 0;
        if (cmd_len >= 5) {
            since = rd_le32(&cmd[1]);
        }
        ctx->contact_iter_active = true;
        ctx->contact_iter_since = since;
        ctx->contact_iter_idx = 0;
        ctx->contact_iter_lastmod = since;

        if (env->send_frame_cb) {
            uint8_t start_frame[5];
            start_frame[0] = RESP_CODE_CONTACTS_START;
            wr_le32(&start_frame[1], (uint32_t)ctx->contact_count);
            env->send_frame_cb(start_frame, 5, env->user_data);
            return 0;
        }

        if (resp_max < 5) return 0;
        resp_out[0] = RESP_CODE_CONTACTS_START;
        wr_le32(&resp_out[1], (uint32_t)ctx->contact_count);
        return 5;
    }

    case CMD_SEND_TXT_MSG: {
        if (cmd_len < 14) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        uint8_t txt_type = cmd[1];
        uint8_t attempt = cmd[2];
        uint32_t msg_ts = rd_le32(&cmd[3]);
        const companion_contact_t *c = meshcore_companion_find_contact_by_prefix(ctx, &cmd[7]);
        if (!c) {
            return write_err(resp_out, resp_max, COMPANION_ERR_NOT_FOUND);
        }

        char text[128];
        size_t tlen = cmd_len - 13;
        if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
        memcpy(text, &cmd[13], tlen);
        text[tlen] = '\0';

        uint32_t now_s = env->get_time_s ? env->get_time_s() : 0;
        bool is_dup = (attempt > 0) ||
                      (ctx->last_tx_msg_ts != 0 &&
                       ctx->last_tx_msg_ts == msg_ts &&
                       strcmp(ctx->last_tx_msg_text, text) == 0 &&
                       (now_s == 0 || ctx->last_tx_seen_s == 0 || (now_s - ctx->last_tx_seen_s) <= 15));

        if (!is_dup) {
            uint8_t ack_tag[4];
            memcpy(ack_tag, &cmd[3], 4);
            if (env->send_direct_txt) {
                if (!env->send_direct_txt(c->pub_key, txt_type, msg_ts, text, ack_tag)) {
                    return write_err(resp_out, resp_max, COMPANION_ERR_BAD_STATE);
                }
            }
            ctx->last_tx_msg_ts = msg_ts;
            copy_cstr(ctx->last_tx_msg_text, sizeof(ctx->last_tx_msg_text), text);
            ctx->last_tx_seen_s = now_s;
            memcpy(ctx->last_tx_ack_tag, ack_tag, 4);
        }

        if (resp_max < 10) return 0;
        resp_out[0] = RESP_CODE_SENT;
        resp_out[1] = (c->out_path_len > 0) ? 0 : 1; /* 0 = direct, 1 = flood */
        memcpy(&resp_out[2], ctx->last_tx_ack_tag, 4);
        wr_le32(&resp_out[6], 5000); /* est_timeout_ms */
        return 10;
    }

    case CMD_SEND_CHANNEL_TXT_MSG: {
        if (cmd_len <= 7) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        uint8_t ch_idx = cmd[2];
        uint32_t msg_ts = rd_le32(&cmd[3]);
        char text[128];
        size_t tlen = cmd_len - 7;
        if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
        memcpy(text, &cmd[7], tlen);
        text[tlen] = '\0';

        uint32_t now_s = env->get_time_s ? env->get_time_s() : 0;
        bool is_dup = (ctx->last_tx_msg_ts != 0 &&
                       ctx->last_tx_msg_ts == msg_ts &&
                       strcmp(ctx->last_tx_msg_text, text) == 0 &&
                       (now_s == 0 || ctx->last_tx_seen_s == 0 || (now_s - ctx->last_tx_seen_s) <= 15));

        if (!is_dup) {
            ctx->last_tx_msg_ts = msg_ts;
            copy_cstr(ctx->last_tx_msg_text, sizeof(ctx->last_tx_msg_text), text);
            ctx->last_tx_seen_s = now_s;
            if (env->send_channel_txt) {
                env->send_channel_txt(ch_idx, text);
            }
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_GET_DEVICE_TIME: {
        if (resp_max < 5) return 0;
        resp_out[0] = RESP_CODE_CURR_TIME;
        uint32_t now = env->get_time_s ? env->get_time_s() : 0;
        wr_le32(&resp_out[1], now);
        return 5;
    }

    case CMD_SET_DEVICE_TIME: {
        if (cmd_len < 5) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        uint32_t epoch = rd_le32(&cmd[1]);
        if (epoch < 1700000000UL) {
            return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        }
        if (env->set_time_s) {
            env->set_time_s(epoch);
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_SEND_SELF_ADVERT: {
        bool flood = (cmd_len >= 2 && cmd[1] == 1);
        if (env->send_advert) {
            env->send_advert(flood);
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_SET_ADVERT_NAME: {
        if (cmd_len < 2) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        size_t nlen = cmd_len - 1;
        if (nlen >= sizeof(cfg->node_name)) nlen = sizeof(cfg->node_name) - 1;
        memcpy(cfg->node_name, &cmd[1], nlen);
        cfg->node_name[nlen] = '\0';
        if (cfg_changed) *cfg_changed = true;
        return write_ok(resp_out, resp_max);
    }

    case CMD_SET_ADVERT_LATLON: {
        if (cmd_len < 9) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        int32_t lat_e6 = (int32_t)rd_le32(&cmd[1]);
        int32_t lon_e6 = (int32_t)rd_le32(&cmd[5]);
        if (lat_e6 >= -90000000 && lat_e6 <= 90000000 && lon_e6 >= -180000000 && lon_e6 <= 180000000) {
            cfg->share_loc = true;
            ctx->lat_e6 = lat_e6;
            ctx->lon_e6 = lon_e6;
            ctx->has_latlon = true;
            if (env->set_latlon) {
                env->set_latlon(lat_e6, lon_e6, env->user_data);
            }
            if (cfg_changed) *cfg_changed = true;
            return write_ok(resp_out, resp_max);
        }
        return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
    }

    case CMD_ADD_UPDATE_CONTACT: {
        if (cmd_len < 1 + 32 + 2 + 1) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        companion_contact_t c;
        memset(&c, 0, sizeof(c));
        size_t idx = 1;
        memcpy(c.pub_key, &cmd[idx], COMPANION_PUB_KEY_SIZE);
        idx += COMPANION_PUB_KEY_SIZE;
        c.type = cmd[idx++];
        c.flags = cmd[idx++];
        c.out_path_len = (int8_t)cmd[idx++];
        if (cmd_len >= idx + COMPANION_MAX_PATH_SIZE) {
            memcpy(c.out_path, &cmd[idx], COMPANION_MAX_PATH_SIZE);
            idx += COMPANION_MAX_PATH_SIZE;
        }
        if (cmd_len >= idx + COMPANION_NAME_SIZE) {
            memcpy(c.name, &cmd[idx], COMPANION_NAME_SIZE);
            c.name[COMPANION_NAME_SIZE - 1] = '\0';
            idx += COMPANION_NAME_SIZE;
        }
        if (cmd_len >= idx + 4) {
            c.last_advert_ts = rd_le32(&cmd[idx]);
            idx += 4;
        }
        if (cmd_len >= idx + 8) {
            c.lat_e6 = (int32_t)rd_le32(&cmd[idx]);
            idx += 4;
            c.lon_e6 = (int32_t)rd_le32(&cmd[idx]);
            idx += 4;
        }
        if (cmd_len >= idx + 4) {
            c.lastmod = rd_le32(&cmd[idx]);
        }
        if (meshcore_companion_add_update_contact(ctx, &c)) {
            if (env && env->on_contacts_changed) {
                env->on_contacts_changed(ctx, env->user_data);
            }
            return write_ok(resp_out, resp_max);
        }
        return write_err(resp_out, resp_max, COMPANION_ERR_TABLE_FULL);
    }

    case CMD_REMOVE_CONTACT: {
        if (cmd_len < 1 + COMPANION_PUB_KEY_SIZE) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        meshcore_companion_remove_contact(ctx, &cmd[1]);
        if (env && env->on_contact_removed) {
            env->on_contact_removed(&cmd[1], env->user_data);
        }
        if (env && env->on_contacts_changed) {
            env->on_contacts_changed(ctx, env->user_data);
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_GET_CONTACT_BY_KEY: {
        if (cmd_len < 1 + COMPANION_PUB_KEY_SIZE) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        const companion_contact_t *c = meshcore_companion_get_contact(ctx, &cmd[1]);
        if (c) {
            return format_contact_frame(c, resp_out, resp_max);
        }
        return write_err(resp_out, resp_max, COMPANION_ERR_NOT_FOUND);
    }

    case CMD_GET_BATT_AND_STORAGE: {
        if (resp_max < 11) return 0;
        resp_out[0] = RESP_CODE_BATT_AND_STORAGE;
        wr_le16(&resp_out[1], env->batt_mv);
        wr_le32(&resp_out[3], env->storage_used_kb);
        wr_le32(&resp_out[7], env->storage_total_kb);
        return 11;
    }

    case CMD_SET_OTHER_PARAMS: {
        if (cmd_len >= 2) ctx->manual_add_contacts = cmd[1];
        if (cmd_len >= 3) ctx->telemetry_mode = cmd[2];
        if (cmd_len >= 4) ctx->advert_loc_policy = cmd[3];
        if (cmd_len >= 5) ctx->multi_acks = cmd[4];
        if (env && env->on_contacts_changed) {
            env->on_contacts_changed(ctx, env->user_data);
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_GET_CUSTOM_VARS: {
        if (resp_max < 1) return 0;
        resp_out[0] = RESP_CODE_CUSTOM_VARS;
        return 1;
    }

    case CMD_SET_CUSTOM_VAR: {
        return write_ok(resp_out, resp_max);
    }

    case CMD_GET_TUNING_PARAMS: {
        if (resp_max < 9) return 0;
        resp_out[0] = RESP_CODE_TUNING_PARAMS;
        wr_le32(&resp_out[1], ctx->rx_delay_base_x1000);
        wr_le32(&resp_out[5], ctx->tx_delay_factor_x1000);
        return 9;
    }

    case CMD_SET_TUNING_PARAMS: {
        if (cmd_len >= 9) {
            ctx->rx_delay_base_x1000 = rd_le32(&cmd[1]);
            ctx->tx_delay_factor_x1000 = rd_le32(&cmd[5]);
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_SET_PATH_HASH_MODE: {
        if (cmd_len >= 3 && cmd[1] == 0 && cmd[2] < 3) {
            cfg->path_hash_mode = cmd[2];
            if (cfg_changed) *cfg_changed = true;
            return write_ok(resp_out, resp_max);
        }
        return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
    }

    case CMD_RUN_CLI_COMMAND: {
        if (cmd_len < 2) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        char cli_in[128];
        size_t in_len = cmd_len - 1;
        if (in_len >= sizeof(cli_in)) in_len = sizeof(cli_in) - 1;
        memcpy(cli_in, &cmd[1], in_len);
        cli_in[in_len] = '\0';

        char reply_buf[128] = {0};
        if (env->exec_cli) {
            env->exec_cli(cli_in, reply_buf, sizeof(reply_buf));
        }

        size_t rlen = strlen(reply_buf);
        if (resp_max < 1 + rlen) rlen = resp_max - 1;
        resp_out[0] = RESP_CODE_CLI_REPLY;
        memcpy(&resp_out[1], reply_buf, rlen);
        return 1 + rlen;
    }

    case CMD_GET_AUTOADD_CONFIG: {
        if (resp_max < 3) return 0;
        resp_out[0] = RESP_CODE_AUTOADD_CONFIG;
        resp_out[1] = ctx->autoadd_config;
        resp_out[2] = ctx->autoadd_max_hops;
        return 3;
    }

    case CMD_SET_AUTOADD_CONFIG: {
        if (cmd_len >= 2) ctx->autoadd_config = cmd[1];
        if (cmd_len >= 3) ctx->autoadd_max_hops = (cmd[2] > 64) ? 64 : cmd[2];
        if (env && env->on_contacts_changed) {
            env->on_contacts_changed(ctx, env->user_data);
        }
        return write_ok(resp_out, resp_max);
    }

    case CMD_GET_DEFAULT_FLOOD_SCOPE: {
        if (resp_max < 1) return 0;
        resp_out[0] = RESP_CODE_DEFAULT_FLOOD_SCOPE;
        return 1;
    }

    case CMD_SET_DEFAULT_FLOOD_SCOPE:
        return write_ok(resp_out, resp_max);

    case CMD_GET_ALLOWED_REPEAT_FREQ: {
        if (resp_max < 25) return 0;
        resp_out[0] = RESP_ALLOWED_REPEAT_FREQ;
        wr_le32(&resp_out[1], 433000);
        wr_le32(&resp_out[5], 433000);
        wr_le32(&resp_out[9], 869495);
        wr_le32(&resp_out[13], 869495);
        wr_le32(&resp_out[17], 918000);
        wr_le32(&resp_out[21], 918000);
        return 25;
    }

    case CMD_EXPORT_PRIVATE_KEY:
    case CMD_IMPORT_PRIVATE_KEY:
        return write_disabled(resp_out, resp_max);

    case CMD_RESET_PATH: {
        if (cmd_len < 1 + COMPANION_PUB_KEY_SIZE) return write_err(resp_out, resp_max, COMPANION_ERR_ILLEGAL_ARG);
        companion_contact_t *c = meshcore_companion_get_contact_mut(ctx, &cmd[1]);
        if (c) {
            c->out_path_len = -1;
            memset(c->out_path, 0, COMPANION_MAX_PATH_SIZE);
            if (env && env->on_contacts_changed) {
                env->on_contacts_changed(ctx, env->user_data);
            }
            return write_ok(resp_out, resp_max);
        }
        return write_err(resp_out, resp_max, COMPANION_ERR_NOT_FOUND);
    }

    case CMD_SYNC_NEXT_MESSAGE: {
        if (resp_max < 1) return 0;
        size_t mlen = 0;
        if (env->pop_msg && env->pop_msg(resp_out, resp_max, &mlen) && mlen > 0) {
            return mlen;
        }
        resp_out[0] = RESP_CODE_NO_MORE_MESSAGES;
        return 1;
    }

    default:
        return write_err(resp_out, resp_max, COMPANION_ERR_UNSUPPORTED_CMD);
    }
}
