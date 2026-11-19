#include "meshcore_companion.h"
#include "unity.h"
#include <string.h>

static uint32_t s_mock_time_s = 1000;
static uint32_t mock_get_time_s(void)
{
    return s_mock_time_s;
}

static char s_last_sent_channel_txt[128];
static uint8_t s_last_sent_channel_idx = 0;
static int s_send_channel_txt_calls = 0;

static bool mock_send_channel_txt(uint8_t ch_idx, const char *text)
{
    s_send_channel_txt_calls++;
    s_last_sent_channel_idx = ch_idx;
    if (text) {
        strncpy(s_last_sent_channel_txt, text, sizeof(s_last_sent_channel_txt) - 1);
        s_last_sent_channel_txt[sizeof(s_last_sent_channel_txt) - 1] = '\0';
    } else {
        s_last_sent_channel_txt[0] = '\0';
    }
    return true;
}

static int s_send_direct_txt_calls = 0;
static char s_last_sent_direct_txt[128];
static const uint8_t s_mock_direct_ack_tag[4] = { 0x11, 0x22, 0x33, 0x44 };
static bool mock_send_direct_txt(const uint8_t pub_key[32], uint8_t txt_type, uint32_t msg_ts,
                                 const char *text, uint8_t out_ack_tag[4])
{
    (void)pub_key;
    (void)txt_type;
    (void)msg_ts;
    s_send_direct_txt_calls++;
    if (text) {
        strncpy(s_last_sent_direct_txt, text, sizeof(s_last_sent_direct_txt) - 1);
        s_last_sent_direct_txt[sizeof(s_last_sent_direct_txt) - 1] = '\0';
    } else {
        s_last_sent_direct_txt[0] = '\0';
    }
    if (out_ack_tag) {
        memcpy(out_ack_tag, s_mock_direct_ack_tag, 4);
    }
    return true;
}

static int s_set_latlon_calls = 0;
static int32_t s_last_lat_e6 = 0;
static int32_t s_last_lon_e6 = 0;
static void mock_set_latlon(int32_t lat_e6, int32_t lon_e6, void *user_data)
{
    (void)user_data;
    s_set_latlon_calls++;
    s_last_lat_e6 = lat_e6;
    s_last_lon_e6 = lon_e6;
}

void setUp(void)
{
    s_mock_time_s = 1000;
    s_send_channel_txt_calls = 0;
    s_last_sent_channel_txt[0] = '\0';
    s_last_sent_channel_idx = 0;
    s_send_direct_txt_calls = 0;
    s_last_sent_direct_txt[0] = '\0';
    s_set_latlon_calls = 0;
    s_last_lat_e6 = 0;
    s_last_lon_e6 = 0;
}

void tearDown(void)
{
}

void test_companion_channel_txt_retry_deduplication(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    bool changed = false;

    companion_env_t env = {
        .get_time_s = mock_get_time_s,
        .send_channel_txt = mock_send_channel_txt,
    };

    uint8_t resp[256];
    uint8_t cmd[128];
    cmd[0] = CMD_SEND_CHANNEL_TXT_MSG;
    cmd[1] = 0; /* txt_type */
    cmd[2] = 2; /* ch_idx */
    uint32_t ts = 1700000000;
    memcpy(&cmd[3], &ts, 4);
    const char *text = "MeshCore Channel Deduplication Test";
    size_t tlen = strlen(text);
    memcpy(&cmd[7], text, tlen);

    /* 1. First send: must call send_channel_txt and return RESP_CODE_OK */
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 7 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_INT(1, s_send_channel_txt_calls);
    TEST_ASSERT_EQUAL_UINT8(2, s_last_sent_channel_idx);
    TEST_ASSERT_EQUAL_STRING(text, s_last_sent_channel_txt);

    /* 2. Immediate retry: same msg_ts and text within 15s */
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 7 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_INT(1, s_send_channel_txt_calls); /* NOT called again! */

    /* 3. Retry 5 seconds later: still deduplicated */
    s_mock_time_s = 1005;
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 7 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_INT(1, s_send_channel_txt_calls); /* Still 1! */

    /* 4. New message (different timestamp): must be sent */
    ts = 1700000001;
    memcpy(&cmd[3], &ts, 4);
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 7 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_INT(2, s_send_channel_txt_calls); /* Increment to 2 */
}

void test_companion_txt_msg_attempt_retry_deduplication(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    bool changed = false;

    companion_env_t env = {
        .get_time_s = mock_get_time_s,
        .send_direct_txt = mock_send_direct_txt,
    };

    uint8_t resp[256];
    uint8_t cmd[128];
    cmd[0] = CMD_SEND_TXT_MSG;
    cmd[1] = 0; /* txt_type */
    cmd[2] = 0; /* attempt = 0 */
    uint32_t ts = 1700000050;
    memcpy(&cmd[3], &ts, 4);
    for (int i = 0; i < 6; i++) cmd[7 + i] = (uint8_t)(0x30 + i); /* dest prefix */
    const char *text = "Direct Msg Deduplication Test";
    size_t tlen = strlen(text);
    memcpy(&cmd[13], text, tlen);

    /* 0. Unknown prefix must fail with COMPANION_ERR_NOT_FOUND */
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 13 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(COMPANION_ERR_NOT_FOUND, resp[1]);
    TEST_ASSERT_EQUAL_INT(0, s_send_direct_txt_calls);

    /* Register contact with matching prefix and a known direct path */
    companion_contact_t contact = { .out_path_len = 2 };
    for (int i = 0; i < 32; i++) contact.pub_key[i] = (uint8_t)(0x30 + i);
    strncpy(contact.name, "Peer30", sizeof(contact.name) - 1);
    TEST_ASSERT_TRUE(meshcore_companion_add_update_contact(&ctx, &contact));

    /* 1. Initial attempt = 0: must call send_direct_txt and return RESP_CODE_SENT */
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 13 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(10, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SENT, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(0, resp[1]); /* direct route since out_path_len == 2 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_mock_direct_ack_tag, &resp[2], 4);
    TEST_ASSERT_EQUAL_INT(1, s_send_direct_txt_calls);
    TEST_ASSERT_EQUAL_STRING(text, s_last_sent_direct_txt);

    /* 2. Retry attempt = 1: companion app retransmitting; must NOT call send_direct_txt */
    cmd[2] = 1;
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 13 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(10, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SENT, resp[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_mock_direct_ack_tag, &resp[2], 4);
    TEST_ASSERT_EQUAL_INT(1, s_send_direct_txt_calls); /* Still 1! */

    /* 3. Retry attempt = 2: must NOT call send_direct_txt */
    cmd[2] = 2;
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 13 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(10, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SENT, resp[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_mock_direct_ack_tag, &resp[2], 4);
    TEST_ASSERT_EQUAL_INT(1, s_send_direct_txt_calls); /* Still 1! */
}

void test_companion_battery_reporting_4190mv(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    bool changed = false;

    companion_env_t env = {
        .batt_mv = 4190,
        .storage_used_kb = 256,
        .storage_total_kb = 4096,
        .get_time_s = mock_get_time_s,
    };

    uint8_t resp[256];

    /* 1. CMD_GET_BATT_AND_STORAGE (20) */
    uint8_t cmd_batt[] = { CMD_GET_BATT_AND_STORAGE };
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_batt, sizeof(cmd_batt), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(11, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_BATT_AND_STORAGE, resp[0]);
    uint16_t batt_mv;
    memcpy(&batt_mv, &resp[1], 2);
    TEST_ASSERT_EQUAL_UINT16(4190, batt_mv);

    /* 2. CMD_GET_STATS with STATS_TYPE_CORE (56, 0) */
    uint8_t cmd_stats[] = { CMD_GET_STATS, STATS_TYPE_CORE };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_stats, sizeof(cmd_stats), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(11, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_STATS, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(STATS_TYPE_CORE, resp[1]);
    memcpy(&batt_mv, &resp[2], 2);
    TEST_ASSERT_EQUAL_UINT16(4190, batt_mv);
}

void test_companion_get_channel_slots_0_to_7_and_not_found_on_8(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    bool changed = false;

    companion_env_t env = {
        .get_time_s = mock_get_time_s,
    };

    uint8_t resp[256];

    /* Slot 0: valid, Public channel */
    uint8_t cmd0[] = { CMD_GET_CHANNEL, 0 };
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd0, sizeof(cmd0), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(50, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CHANNEL_INFO, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(0, resp[1]);
    TEST_ASSERT_EQUAL_STRING("Public", (char *)&resp[2]);

    /* Slots 1..7: must return RESP_CODE_CHANNEL_INFO with empty name and zero secret */
    for (uint8_t idx = 1; idx < COMPANION_MAX_CHANNELS; idx++) {
        uint8_t cmd[] = { CMD_GET_CHANNEL, idx };
        len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, sizeof(cmd), resp, sizeof(resp));
        TEST_ASSERT_EQUAL_INT(50, (int)len);
        TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CHANNEL_INFO, resp[0]);
        TEST_ASSERT_EQUAL_UINT8(idx, resp[1]);
        TEST_ASSERT_EQUAL_UINT8(0, resp[2]); /* name is empty */
        for (int s = 0; s < 16; s++) {
            TEST_ASSERT_EQUAL_UINT8(0, resp[34 + s]); /* secret is zeroed */
        }
    }

    /* Slot 8: out of bounds -> COMPANION_ERR_NOT_FOUND */
    uint8_t cmd8[] = { CMD_GET_CHANNEL, COMPANION_MAX_CHANNELS };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd8, sizeof(cmd8), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(COMPANION_ERR_NOT_FOUND, resp[1]);
}

static int s_channels_changed_count = 0;
static void mock_on_channels_changed(companion_context_t *ctx, void *user_data)
{
    (void)ctx;
    (void)user_data;
    s_channels_changed_count++;
}

void test_companion_set_channel_and_delete(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    bool changed = false;
    s_channels_changed_count = 0;

    companion_env_t env = {
        .get_time_s = mock_get_time_s,
        .on_channels_changed = mock_on_channels_changed,
    };

    uint8_t resp[256];
    uint8_t cmd_set[2 + 32 + 16] = {0};
    cmd_set[0] = CMD_SET_CHANNEL;
    cmd_set[1] = 1; /* slot 1 */
    strncpy((char *)&cmd_set[2], "Emergency", 31);
    memset(&cmd_set[34], 0xAB, 16);

    /* Set channel 1 */
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_set, sizeof(cmd_set), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_INT(1, s_channels_changed_count);
    TEST_ASSERT_TRUE(ctx.channels[1].valid);
    TEST_ASSERT_EQUAL_STRING("Emergency", ctx.channels[1].name);

    /* Verify GET_CHANNEL on slot 1 */
    uint8_t cmd_get[] = { CMD_GET_CHANNEL, 1 };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_get, sizeof(cmd_get), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(50, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CHANNEL_INFO, resp[0]);
    TEST_ASSERT_EQUAL_STRING("Emergency", (char *)&resp[2]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, resp[34]);

    /* Delete channel 1 (empty name + all-zero secret) */
    memset(&cmd_set[2], 0, 32);
    memset(&cmd_set[34], 0, 16);
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_set, sizeof(cmd_set), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_INT(2, s_channels_changed_count);
    TEST_ASSERT_FALSE(ctx.channels[1].valid);

    /* Verify GET_CHANNEL on slot 1 returns empty name */
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_get, sizeof(cmd_get), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(50, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CHANNEL_INFO, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(0, resp[2]);
}

void test_companion_latlon_and_unsupported_cmds(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = { .freq_khz = 869618, .bw_hz = 62500, .sf = 8, .cr = 5, .tx_power_dbm = 22, .node_name = "LatLonNode" };
    companion_env_t env = {
        .lat_e6 = 1000000,
        .lon_e6 = 2000000,
        .set_latlon = mock_set_latlon,
    };
    uint8_t resp[256];
    bool changed = false;

    uint8_t latlon_cmd[9] = { CMD_SET_ADVERT_LATLON };
    int32_t lat_e6 = 50061400;
    int32_t lon_e6 = 19936600;
    memcpy(&latlon_cmd[1], &lat_e6, 4);
    memcpy(&latlon_cmd[5], &lon_e6, 4);

    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, latlon_cmd, sizeof(latlon_cmd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_TRUE(cfg.share_loc);
    TEST_ASSERT_EQUAL_INT(1, s_set_latlon_calls);
    TEST_ASSERT_EQUAL_INT32(lat_e6, s_last_lat_e6);
    TEST_ASSERT_EQUAL_INT32(lon_e6, s_last_lon_e6);

    uint8_t app_start[] = { CMD_APP_START };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, app_start, sizeof(app_start), resp, sizeof(resp));
    TEST_ASSERT_TRUE(len >= 58);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SELF_INFO, resp[0]);
    int32_t ret_lat = 0, ret_lon = 0;
    memcpy(&ret_lat, &resp[36], 4);
    memcpy(&ret_lon, &resp[40], 4);
    TEST_ASSERT_EQUAL_INT32(lat_e6, ret_lat);
    TEST_ASSERT_EQUAL_INT32(lon_e6, ret_lon);

    const uint8_t unsupported[] = { CMD_SHARE_CONTACT, CMD_REBOOT, CMD_SEND_TRACE_PATH, CMD_SEND_CONTROL_DATA };
    for (size_t i = 0; i < sizeof(unsupported); i++) {
        uint8_t cmd[1] = { unsupported[i] };
        len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, sizeof(cmd), resp, sizeof(resp));
        TEST_ASSERT_EQUAL_INT(2, (int)len);
        TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
        TEST_ASSERT_EQUAL_UINT8(COMPANION_ERR_UNSUPPORTED_CMD, resp[1]);
    }
}

static void test_companion_set_channel_slot0_syncs_cfg(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);

    companion_env_t env = {0};
    lora_config_t cfg = {0};
    bool cfg_changed = false;

    uint8_t resp[256];
    uint8_t cmd[2 + 32 + 16] = {0};
    cmd[0] = CMD_SET_CHANNEL;
    cmd[1] = 0; /* slot 0 */
    memcpy(&cmd[2], "Emergency", 9);
    for (int i = 0; i < 16; i++) {
        cmd[34 + i] = (uint8_t)(i + 1);
    }

    size_t rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &cfg_changed,
                                                cmd, sizeof(cmd),
                                                resp, sizeof(resp));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)rlen);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_TRUE(cfg_changed);
    TEST_ASSERT_EQUAL_STRING("Emergency", cfg.channel_name);
    TEST_ASSERT_TRUE(strlen(cfg.channel_psk) > 0);
}

static void build_add_update_contact_cmd(uint8_t *cmd, size_t *out_len,
                                         const uint8_t pub_key[COMPANION_PUB_KEY_SIZE],
                                         uint8_t type, uint8_t flags, int8_t out_path_len,
                                         const char *name, int32_t lat_e6, int32_t lon_e6)
{
    size_t idx = 0;
    cmd[idx++] = CMD_ADD_UPDATE_CONTACT;
    memcpy(&cmd[idx], pub_key, COMPANION_PUB_KEY_SIZE);
    idx += COMPANION_PUB_KEY_SIZE;
    cmd[idx++] = type;
    cmd[idx++] = flags;
    cmd[idx++] = (uint8_t)out_path_len;
    memset(&cmd[idx], 0, COMPANION_MAX_PATH_SIZE);
    idx += COMPANION_MAX_PATH_SIZE;
    memset(&cmd[idx], 0, COMPANION_NAME_SIZE);
    if (name) {
        strncpy((char *)&cmd[idx], name, COMPANION_NAME_SIZE - 1);
    }
    idx += COMPANION_NAME_SIZE;
    uint32_t ts = 1700000000U;
    memcpy(&cmd[idx], &ts, 4);
    idx += 4;
    memcpy(&cmd[idx], &lat_e6, 4);
    idx += 4;
    memcpy(&cmd[idx], &lon_e6, 4);
    idx += 4;
    memcpy(&cmd[idx], &ts, 4);
    idx += 4;
    *out_len = idx;
}

static void test_companion_contacts_add_reset_remove_and_max64(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    companion_env_t env = { .get_time_s = mock_get_time_s };
    lora_config_t cfg = {0};
    bool changed = false;

    uint8_t resp[256];
    uint8_t cmd[256];
    size_t cmd_len = 0;

    /* 1. Add contact #0 and verify fields persisted in ctx.contacts */
    uint8_t key0[COMPANION_PUB_KEY_SIZE] = {0};
    key0[0] = 0xA0;
    key0[31] = 0x01;
    build_add_update_contact_cmd(cmd, &cmd_len, key0, 1, 0x02, 3, "Alice", 50061400, 19936600);
    size_t rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, cmd_len, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)rlen);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.contact_count);
    TEST_ASSERT_EQUAL_STRING("Alice", ctx.contacts[0].name);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.contacts[0].type);
    TEST_ASSERT_EQUAL_INT8(3, ctx.contacts[0].out_path_len);
    TEST_ASSERT_EQUAL_INT32(50061400, ctx.contacts[0].lat_e6);
    TEST_ASSERT_EQUAL_INT32(19936600, ctx.contacts[0].lon_e6);

    /* 2. Update existing contact with same pub_key: updates fields without increasing count */
    build_add_update_contact_cmd(cmd, &cmd_len, key0, 2, 0x04, 5, "Alice-Updated", 52229700, 21012200);
    rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, cmd_len, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)rlen);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(1, ctx.contact_count);
    TEST_ASSERT_EQUAL_STRING("Alice-Updated", ctx.contacts[0].name);
    TEST_ASSERT_EQUAL_UINT8(2, ctx.contacts[0].type);
    TEST_ASSERT_EQUAL_INT8(5, ctx.contacts[0].out_path_len);
    TEST_ASSERT_EQUAL_INT32(52229700, ctx.contacts[0].lat_e6);
    TEST_ASSERT_EQUAL_INT32(21012200, ctx.contacts[0].lon_e6);

    /* 3. Fill up to COMPANION_MAX_CONTACTS (64) */
    for (uint8_t i = 1; i < COMPANION_MAX_CONTACTS; i++) {
        uint8_t key[COMPANION_PUB_KEY_SIZE] = {0};
        key[0] = 0xB0;
        key[31] = i;
        build_add_update_contact_cmd(cmd, &cmd_len, key, 1, 0, 1, "Peer", 0, 0);
        rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, cmd_len, resp, sizeof(resp));
        TEST_ASSERT_EQUAL_INT(1, (int)rlen);
        TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    }
    TEST_ASSERT_EQUAL_UINT8(COMPANION_MAX_CONTACTS, ctx.contact_count);

    /* 4. 65th distinct contact -> RESP_CODE_ERR (COMPANION_ERR_TABLE_FULL) */
    uint8_t key65[COMPANION_PUB_KEY_SIZE] = {0};
    key65[0] = 0xC0;
    key65[31] = 0xFF;
    build_add_update_contact_cmd(cmd, &cmd_len, key65, 1, 0, 1, "Overflow", 0, 0);
    rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, cmd_len, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)rlen);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(COMPANION_ERR_TABLE_FULL, resp[1]);
    TEST_ASSERT_EQUAL_UINT8(COMPANION_MAX_CONTACTS, ctx.contact_count);

    /* 5. CMD_RESET_PATH on contact #0 -> out_path_len == -1 and RESP_CODE_OK */
    uint8_t reset_cmd[1 + COMPANION_PUB_KEY_SIZE];
    reset_cmd[0] = CMD_RESET_PATH;
    memcpy(&reset_cmd[1], key0, COMPANION_PUB_KEY_SIZE);
    rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, reset_cmd, sizeof(reset_cmd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)rlen);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_INT8(-1, ctx.contacts[0].out_path_len);

    /* 6. CMD_REMOVE_CONTACT -> count decrements and slot can be reused */
    uint8_t rem_cmd[1 + COMPANION_PUB_KEY_SIZE];
    rem_cmd[0] = CMD_REMOVE_CONTACT;
    memcpy(&rem_cmd[1], key0, COMPANION_PUB_KEY_SIZE);
    rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, rem_cmd, sizeof(rem_cmd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)rlen);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(COMPANION_MAX_CONTACTS - 1, ctx.contact_count);

    build_add_update_contact_cmd(cmd, &cmd_len, key65, 1, 0, 2, "Reused", 0, 0);
    rlen = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, cmd_len, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)rlen);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(COMPANION_MAX_CONTACTS, ctx.contact_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_companion_channel_txt_retry_deduplication);
    RUN_TEST(test_companion_txt_msg_attempt_retry_deduplication);
    RUN_TEST(test_companion_latlon_and_unsupported_cmds);
    RUN_TEST(test_companion_battery_reporting_4190mv);
    RUN_TEST(test_companion_get_channel_slots_0_to_7_and_not_found_on_8);
    RUN_TEST(test_companion_set_channel_and_delete);
    RUN_TEST(test_companion_set_channel_slot0_syncs_cfg);
    RUN_TEST(test_companion_contacts_add_reset_remove_and_max64);
    return UNITY_END();
}
