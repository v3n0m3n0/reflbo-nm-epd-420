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

void setUp(void)
{
    s_mock_time_s = 1000;
    s_send_channel_txt_calls = 0;
    s_last_sent_channel_txt[0] = '\0';
    s_last_sent_channel_idx = 0;
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
        .send_channel_txt = mock_send_channel_txt,
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

    /* 1. Initial attempt = 0: must call send_channel_txt and return RESP_CODE_SENT */
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 13 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(10, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SENT, resp[0]);
    TEST_ASSERT_EQUAL_INT(1, s_send_channel_txt_calls);
    TEST_ASSERT_EQUAL_STRING(text, s_last_sent_channel_txt);

    /* 2. Retry attempt = 1: companion app retransmitting; must NOT call send_channel_txt */
    cmd[2] = 1;
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 13 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(10, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SENT, resp[0]);
    TEST_ASSERT_EQUAL_INT(1, s_send_channel_txt_calls); /* Still 1! */

    /* 3. Retry attempt = 2: must NOT call send_channel_txt */
    cmd[2] = 2;
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, 13 + tlen, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(10, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SENT, resp[0]);
    TEST_ASSERT_EQUAL_INT(1, s_send_channel_txt_calls); /* Still 1! */
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_companion_channel_txt_retry_deduplication);
    RUN_TEST(test_companion_txt_msg_attempt_retry_deduplication);
    RUN_TEST(test_companion_battery_reporting_4190mv);
    return UNITY_END();
}
