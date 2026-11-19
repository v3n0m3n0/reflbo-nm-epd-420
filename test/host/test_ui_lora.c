#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include "context_fixtures.h"
#include "gfx.h"
#include "gfx_fonts.h"
#include "lang.h"
#include "ui_internal.h"
#include "ui_lora.h"
#include "unity.h"

typedef ui_lora_t ui_lora_data_t;

static uint8_t s_buf[400 * 300 / 8];
static gfx_fb_t s_fb;
static ui_context_t s_ctx;
static ui_lora_data_t s_lora;

void setUp(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    gfx_fb_init(&s_fb, s_buf, 400, 300);
    gfx_clear(&s_fb, GFX_WHITE);

    s_ctx = fixture_context();
    memset(&s_lora, 0, sizeof(s_lora));
    s_lora.enabled = true;
    s_lora.initialized = true;
    s_lora.freq_khz = 868000;
    s_lora.bw_hz = 250000;
    s_lora.sf = 7;
    s_lora.cr = 5;
    snprintf(s_lora.node_name, sizeof(s_lora.node_name), "TestNode");
    snprintf(s_lora.channel_name, sizeof(s_lora.channel_name), "TestCh");
    s_ctx.lora = &s_lora;
}

void tearDown(void) {}

static int count_black_pixels_in_rect(const gfx_fb_t *fb, int x0, int y0, int x1, int y1)
{
    int count = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (gfx_get_pixel(fb, x, y)) {
                count++;
            }
        }
    }
    return count;
}

static void test_ui_lora_disabled(void)
{
    s_lora.enabled = false;
    gfx_rect_t below = { 0, 20, 400, 280 };
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    /* When disabled, displays LoRa Off message in centre */
    int black = count_black_pixels_in_rect(&s_fb, 0, 20, 400, 300);
    TEST_ASSERT_TRUE(black > 0);
}

static void test_ui_lora_no_messages(void)
{
    s_lora.msg_count = 0;
    gfx_rect_t below = { 0, 20, 400, 280 };
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    /* Middle section (y = 76..180) shows "No messages yet" */
    int middle_black = count_black_pixels_in_rect(&s_fb, 8, 100, 392, 130);
    TEST_ASSERT_TRUE(middle_black > 0);
}

static void test_ui_lora_multiline_message_wrapping(void)
{
    gfx_rect_t below = { 0, 20, 400, 280 };

    /* --- Step 1: Render baseline with a short 1-line message --- */
    s_lora.msg_count = 1;
    s_lora.msgs[0].timestamp = 1727286480; /* valid timestamp >= 2020 */
    s_lora.msgs[0].outgoing = false;
    s_lora.msgs[0].is_direct = false;
    snprintf(s_lora.msgs[0].sender, sizeof(s_lora.msgs[0].sender), "Bob");
    snprintf(s_lora.msgs[0].text, sizeof(s_lora.msgs[0].text), "Short test");

    ui_draw_lora_view(&s_fb, below, &s_ctx);

    /* Line 1 band (y = 76..89) must have rendered text */
    int base_line1_pixels = count_black_pixels_in_rect(&s_fb, 8, 76, 392, 89);
    TEST_ASSERT_TRUE(base_line1_pixels > 0);

    /* Line 2 band (y = 89..102) has at most minor descender overhang (<= 5 px) from line 1 */
    int base_line2_pixels = count_black_pixels_in_rect(&s_fb, 8, 89, 392, 102);
    int base_line3_pixels = count_black_pixels_in_rect(&s_fb, 8, 102, 392, 115);
    TEST_ASSERT_TRUE(base_line2_pixels <= 5);
    TEST_ASSERT_EQUAL_INT(0, base_line3_pixels);

    /* --- Step 2: Render with long multi-line message (~130 chars, spanning 3 lines) --- */
    gfx_clear(&s_fb, GFX_WHITE);
    memset(&s_lora.msgs[0], 0, sizeof(s_lora.msgs[0]));
    s_lora.msg_count = 1;
    s_lora.msgs[0].timestamp = 1727286480;
    s_lora.msgs[0].outgoing = false;
    s_lora.msgs[0].is_direct = false;
    snprintf(s_lora.msgs[0].sender, sizeof(s_lora.msgs[0].sender), "NodeAlpha");
    snprintf(s_lora.msgs[0].text, sizeof(s_lora.msgs[0].text),
             "To jest bardzo dluga wiadomosc testowa z sieci MeshCore ktora nie miesci sie w jednej linii na ekranie e-paper i wymaga zawijania wierszy");

    ui_draw_lora_view(&s_fb, below, &s_ctx);

    /* Check black pixel counts in all three line bands */
    int ml_line1_pixels = count_black_pixels_in_rect(&s_fb, 8, 76, 392, 89);
    int ml_line2_pixels = count_black_pixels_in_rect(&s_fb, 8, 89, 392, 102);
    int ml_line3_pixels = count_black_pixels_in_rect(&s_fb, 8, 102, 392, 115);

    /* Line 1, Line 2, and Line 3 must all have significant rendered pixels */
    TEST_ASSERT_TRUE(ml_line1_pixels > 50);
    TEST_ASSERT_TRUE(ml_line2_pixels > 50);
    TEST_ASSERT_TRUE(ml_line3_pixels > 50);

    /* Line 4 band (y = 118..131) must have NO pixels, as max lines is 3 */
    int ml_line4_pixels = count_black_pixels_in_rect(&s_fb, 8, 118, 392, 131);
    TEST_ASSERT_EQUAL_INT(0, ml_line4_pixels);
}

static void test_ui_lora_unbroken_long_word_wrapping(void)
{
    gfx_rect_t below = { 0, 20, 400, 280 };
    gfx_clear(&s_fb, GFX_WHITE);

    s_lora.msg_count = 1;
    s_lora.msgs[0].timestamp = 1727286480;
    s_lora.msgs[0].outgoing = true;
    s_lora.msgs[0].sender[0] = '\0';
    /* Word without spaces that exceeds 384 px */
    snprintf(s_lora.msgs[0].text, sizeof(s_lora.msgs[0].text),
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz");

    ui_draw_lora_view(&s_fb, below, &s_ctx);

    int ml_line1_pixels = count_black_pixels_in_rect(&s_fb, 8, 76, 392, 89);
    int ml_line2_pixels = count_black_pixels_in_rect(&s_fb, 8, 89, 392, 102);

    TEST_ASSERT_TRUE(ml_line1_pixels > 50);
    TEST_ASSERT_TRUE(ml_line2_pixels > 50);
}

static void test_ui_lora_chronological_scrolling_keeps_newest(void)
{
    gfx_rect_t below = { 0, 20, 400, 280 };
    gfx_clear(&s_fb, GFX_WHITE);

    /* 6 messages: each 2 lines (~30 px height each).
     * 6 * 29 = 174 px > 129 px budget.
     * Backwards fit: msg 5, 4, 3, 2 (4 * 29 = 116 <= 129).
     * Oldest messages 0 and 1 should be scrolled off.
     * Message 5 (newest) must be visible at bottom. */
    s_lora.msg_count = 6;
    for (int i = 0; i < 6; i++) {
        s_lora.msgs[i].timestamp = 1727286480 + (uint32_t)(i * 60);
        s_lora.msgs[i].outgoing = (i % 2 == 0);
        snprintf(s_lora.msgs[i].sender, sizeof(s_lora.msgs[i].sender), "User%d", i);
        snprintf(s_lora.msgs[i].text, sizeof(s_lora.msgs[i].text),
                 "Message number %d with enough text to wrap into two lines cleanly on screen", i);
    }

    ui_draw_lora_view(&s_fb, below, &s_ctx);

    /* Verify pixels exist in the lower portion of the message area (e.g. y = 170..200) */
    int bottom_pixels = count_black_pixels_in_rect(&s_fb, 8, 170, 392, 200);
    TEST_ASSERT_TRUE(bottom_pixels > 50);
}

static bool rect_pixels_equal(const gfx_fb_t *a, const gfx_fb_t *b, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (gfx_get_pixel(a, x, y) != gfx_get_pixel(b, x, y)) {
                return false;
            }
        }
    }
    return true;
}

static void test_ui_lora_negative_fractional_snr_and_zero_pin(void)
{
    static uint8_t expected_buf[400 * 300 / 8];
    gfx_fb_t expected_fb;
    gfx_fb_init(&expected_fb, expected_buf, 400, 300);
    gfx_rect_t below = { 0, 20, 400, 280 };

    /* 1. Verify UI_LORA_MAX_NODES matches the 3 neighbor slots rendered in y=230..299 */
    TEST_ASSERT_EQUAL_INT(3, UI_LORA_MAX_NODES);

    /* 2. Verify ble_pin == 0 displays em-dash in empty messages view rather than PIN: 000000 */
    s_lora.msg_count = 0;
    s_lora.ble_pin = 0;
    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 133, 384, 16 },
                     GFX_ALIGN_CENTER, "BLE: MeshCore-TestNode \xC2\xB7 PIN: \xE2\x80\x94", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 133, 392, 149));

    /* 3. Verify negative fractional SNR values in (-1.0, 0) retain minus sign */
    const struct {
        int8_t snr_q4;
        const char *expected_snr;
    } cases[] = {
        { -1, "-0.2 dB" },
        { -2, "-0.5 dB" },
        { -3, "-0.7 dB" },
        { -4, "-1.0 dB" },
        {  0, "+0.0 dB" },
        {  2, "+0.5 dB" },
    };

    s_lora.packet_count = 1;
    s_lora.last_rssi_dbm = -80;
    s_lora.noise_floor_dbm = -100;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        s_lora.last_snr_q4 = cases[i].snr_q4;
        gfx_clear(&s_fb, GFX_WHITE);
        ui_draw_lora_view(&s_fb, below, &s_ctx);

        char expected_line[64];
        snprintf(expected_line, sizeof(expected_line), "RSSI -80 \xC2\xB7 %s \xC2\xB7 NF -100",
                 cases[i].expected_snr);
        int right_w = gfx_text_width(&gfx_font_sans_12, expected_line);
        int right_box_w = right_w + 2;
        if (right_box_w > 240) right_box_w = 240;
        int right_x = 392 - right_box_w;
        memset(expected_buf, 0, sizeof(expected_buf));
        gfx_clear(&expected_fb, GFX_WHITE);
        gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ right_x, 40, right_box_w, 16 },
                         GFX_ALIGN_RIGHT, expected_line, GFX_BLACK);
        TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, right_x, 40, 392, 56));
    }
}

static void test_ui_status_draws_lora_and_ble_indicator_when_enabled(void)
{
    static uint8_t off_buf[400 * 300 / 8], ble_buf[400 * 300 / 8];
    gfx_fb_t off_fb, ble_fb;
    gfx_fb_init(&off_fb, off_buf, 400, 300);
    gfx_fb_init(&ble_fb, ble_buf, 400, 300);

    ui_preset_t preset;
    memset(&preset, 0, sizeof(preset));
    s_ctx.time_valid = true;
    s_ctx.web_session = false;
    s_ctx.pc = NULL;
    s_ctx.sync = UI_SYNC_IDLE;
    s_ctx.wifi = UI_WIFI_NONE;

    s_ctx.lora = NULL;
    gfx_clear(&off_fb, GFX_WHITE);
    ui_status_draw(&off_fb, &s_ctx, &preset, false);
    TEST_ASSERT_EQUAL_INT(0, count_black_pixels_in_rect(&off_fb, 4, 2, 20, 18));

    s_ctx.lora = &s_lora;
    s_lora.enabled = false;
    gfx_clear(&off_fb, GFX_WHITE);
    ui_status_draw(&off_fb, &s_ctx, &preset, false);
    TEST_ASSERT_EQUAL_INT(0, count_black_pixels_in_rect(&off_fb, 4, 2, 20, 18));

    s_lora.enabled = true;
    s_lora.ble_connected = false;
    gfx_clear(&s_fb, GFX_WHITE);
    ui_status_draw(&s_fb, &s_ctx, &preset, false);
    TEST_ASSERT_TRUE(count_black_pixels_in_rect(&s_fb, 4, 2, 20, 18) > 0);

    s_lora.ble_connected = true;
    gfx_clear(&ble_fb, GFX_WHITE);
    ui_status_draw(&ble_fb, &s_ctx, &preset, false);
    TEST_ASSERT_TRUE(count_black_pixels_in_rect(&ble_fb, 4, 2, 20, 18) > 0);
    TEST_ASSERT_FALSE(rect_pixels_equal(&s_fb, &ble_fb, 4, 2, 20, 18));
}

static void test_ui_lora_dm_outgoing_ack_and_multichannel_badges(void)
{
    static uint8_t expected_buf[400 * 300 / 8];
    gfx_fb_t expected_fb;
    gfx_fb_init(&expected_fb, expected_buf, 400, 300);
    gfx_rect_t below = { 0, 20, 400, 280 };

    /* 1. Outgoing DM with ACK delivered renders "[TX] > @Alice: Secret [OK]" */
    s_lora.msg_count = 1;
    memset(&s_lora.msgs[0], 0, sizeof(s_lora.msgs[0]));
    s_lora.msgs[0].timestamp = 0; /* uses [TX] prefix independent of host timezone */
    s_lora.msgs[0].outgoing = true;
    s_lora.msgs[0].is_direct = true;
    s_lora.msgs[0].ack_status = 2; /* MESHCORE_ACK_DELIVERED */
    snprintf(s_lora.msgs[0].sender, sizeof(s_lora.msgs[0].sender), "Alice");
    snprintf(s_lora.msgs[0].text, sizeof(s_lora.msgs[0].text), "Secret");

    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 61, 250, 16 },
                     GFX_ALIGN_LEFT, lang_str(s_ctx.lang, LS_LORA_MESSAGES), GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 76, 384, 14 },
                     GFX_ALIGN_LEFT, "[TX] > @Alice: Secret [OK]", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 76, 392, 90));

    /* 2. Incoming secondary channel #2 message renders "[RX] < [#2] Bob: Ops msg" */
    memset(&s_lora.msgs[0], 0, sizeof(s_lora.msgs[0]));
    s_lora.msgs[0].timestamp = 0;
    s_lora.msgs[0].outgoing = false;
    s_lora.msgs[0].is_direct = false;
    s_lora.msgs[0].channel_idx = 2;
    snprintf(s_lora.msgs[0].sender, sizeof(s_lora.msgs[0].sender), "Bob");
    snprintf(s_lora.msgs[0].text, sizeof(s_lora.msgs[0].text), "Ops msg");

    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 61, 250, 16 },
                     GFX_ALIGN_LEFT, lang_str(s_ctx.lang, LS_LORA_MESSAGES), GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 76, 384, 14 },
                     GFX_ALIGN_LEFT, "[RX] < [#2] Bob: Ops msg", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 76, 392, 90));
}

static void test_ui_lora_hard_newlines_break_lines_without_missing_glyph_box(void)
{
    static uint8_t expected_buf[400 * 300 / 8];
    gfx_fb_t expected_fb;
    gfx_fb_init(&expected_fb, expected_buf, 400, 300);
    gfx_rect_t below = { 0, 20, 400, 280 };

    /* Short multi-line bot reply ("Path:\n1. C3CC\n2. B9F0\n3. 6FD3") must break at '\n' into 3 clean lines */
    s_lora.msg_count = 1;
    memset(&s_lora.msgs[0], 0, sizeof(s_lora.msgs[0]));
    s_lora.msgs[0].timestamp = 0;
    s_lora.msgs[0].outgoing = false;
    s_lora.msgs[0].is_direct = false;
    s_lora.msgs[0].channel_idx = 2;
    snprintf(s_lora.msgs[0].sender, sizeof(s_lora.msgs[0].sender), "Bot");
    snprintf(s_lora.msgs[0].text, sizeof(s_lora.msgs[0].text), "Path:\r\n1. C3CC\n2. B9F0\n3. 6FD3");

    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 61, 250, 16 },
                     GFX_ALIGN_LEFT, lang_str(s_ctx.lang, LS_LORA_MESSAGES), GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 76, 384, 14 },
                     GFX_ALIGN_LEFT, "[RX] < [#2] Bot: Path:", GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 89, 384, 14 },
                     GFX_ALIGN_LEFT, "1. C3CC", GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 102, 384, 14 },
                     GFX_ALIGN_LEFT, "2. B9F0 3. 6FD3", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 76, 392, 116));

    /* UI_FIELD_LORA_LAST_MSG must also flatten newlines to spaces */
    ui_value_t val = {0};
    TEST_ASSERT_TRUE(ui_resolve_lora(&s_ctx, UI_FIELD_LORA_LAST_MSG, &val));
    TEST_ASSERT_NULL(strchr(val.text, '\n'));
    TEST_ASSERT_NULL(strchr(val.text, '\r'));
    TEST_ASSERT_EQUAL_STRING("Path: 1. C3CC 2. B9F0 3. 6FD3", val.text);
}

static void test_ui_lora_noise_floor_zero_omitted(void)
{
    static uint8_t expected_buf[400 * 300 / 8];
    gfx_fb_t expected_fb;
    gfx_fb_init(&expected_fb, expected_buf, 400, 300);
    gfx_rect_t below = { 0, 20, 400, 280 };

    /* 1. packet_count == 0, noise_floor_dbm == 0 -> "TX +22 dBm" without NF */
    s_lora.noise_floor_dbm = 0;
    s_lora.packet_count = 0;
    s_lora.tx_power_dbm = 22;
    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 248, 40, 144, 16 },
                     GFX_ALIGN_RIGHT, "TX +22 dBm", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 248, 40, 392, 56));

    /* 2. packet_count > 0, noise_floor_dbm == 0 -> "RSSI -85 dBm · +5.0 dB" without NF */
    s_lora.packet_count = 1;
    s_lora.last_rssi_dbm = -85;
    s_lora.last_snr_q4 = 20;
    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 248, 40, 144, 16 },
                     GFX_ALIGN_RIGHT, "RSSI -85 dBm \xC2\xB7 +5.0 dB", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 248, 40, 392, 56));
}

static void test_ui_lora_channel_name_formatting(void)
{
    static uint8_t expected_buf[400 * 300 / 8];
    gfx_fb_t expected_fb;
    gfx_fb_init(&expected_fb, expected_buf, 400, 300);
    gfx_rect_t below = { 0, 20, 400, 280 };

    /* 1. channel_idx = 0 + channel_name = "Public" renders "[RX] < [Public] Bob: Hello" */
    s_lora.msg_count = 1;
    memset(&s_lora.msgs[0], 0, sizeof(s_lora.msgs[0]));
    s_lora.msgs[0].timestamp = 0;
    s_lora.msgs[0].outgoing = false;
    s_lora.msgs[0].is_direct = false;
    s_lora.msgs[0].channel_idx = 0;
    snprintf(s_lora.msgs[0].channel_name, sizeof(s_lora.msgs[0].channel_name), "Public");
    snprintf(s_lora.msgs[0].sender, sizeof(s_lora.msgs[0].sender), "Bob");
    snprintf(s_lora.msgs[0].text, sizeof(s_lora.msgs[0].text), "Hello");

    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 61, 250, 16 },
                     GFX_ALIGN_LEFT, lang_str(s_ctx.lang, LS_LORA_MESSAGES), GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 76, 384, 14 },
                     GFX_ALIGN_LEFT, "[RX] < [Public] Bob: Hello", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 76, 392, 90));

    /* 2. channel_idx = 2 + channel_name = "#bot" renders "[RX] < [#bot] Bob: Hello" */
    s_lora.msgs[0].channel_idx = 2;
    snprintf(s_lora.msgs[0].channel_name, sizeof(s_lora.msgs[0].channel_name), "#bot");
    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 61, 250, 16 },
                     GFX_ALIGN_LEFT, lang_str(s_ctx.lang, LS_LORA_MESSAGES), GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 76, 384, 14 },
                     GFX_ALIGN_LEFT, "[RX] < [#bot] Bob: Hello", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 76, 392, 90));

    /* 3. channel_idx = 2 + channel_name = "Ops" renders "[RX] < [Ops] Bob: Hello" */
    snprintf(s_lora.msgs[0].channel_name, sizeof(s_lora.msgs[0].channel_name), "Ops");
    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 61, 250, 16 },
                     GFX_ALIGN_LEFT, lang_str(s_ctx.lang, LS_LORA_MESSAGES), GFX_BLACK);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 76, 384, 14 },
                     GFX_ALIGN_LEFT, "[RX] < [Ops] Bob: Hello", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 76, 392, 90));
}

static void test_ui_lora_header_line2_channel_all_and_repeat_off(void)
{
    static uint8_t expected_buf[400 * 300];
    gfx_fb_t expected_fb;
    gfx_fb_init(&expected_fb, expected_buf, 400, 300);

    gfx_rect_t below = { 0, 21, 400, 279 };

    /* 1. active_channel_count = 3 -> "Ch: All", client_repeat = false -> "Rpt: off" even if sent_flood > 0 */
    s_lora.active_channel_count = 3;
    snprintf(s_lora.channel_name, sizeof(s_lora.channel_name), "#test");
    s_lora.client_repeat = false;
    s_lora.sent_flood = 5;
    s_lora.forwarded_count = 2;
    s_lora.packet_count = 0;
    s_lora.packets_sent = 0;

    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 40, 220, 16 },
                     GFX_ALIGN_LEFT, "Ch: All \xC2\xB7 RX: 0 \xC2\xB7 TX: 0 \xC2\xB7 Rpt: off", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 40, 210, 56));

    /* 2. active_channel_count = 1 -> "Ch: #test", client_repeat = true -> "Fwd: 2" */
    s_lora.active_channel_count = 1;
    s_lora.client_repeat = true;

    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ 8, 40, 220, 16 },
                     GFX_ALIGN_LEFT, "Ch: #test \xC2\xB7 RX: 0 \xC2\xB7 TX: 0 \xC2\xB7 Fwd: 2", GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, 8, 40, 210, 56));
}

static void test_ui_lora_header_line2_no_overlap_on_long_rssi(void)
{
    static uint8_t expected_buf[400 * 300];
    gfx_fb_t expected_fb;
    gfx_fb_init(&expected_fb, expected_buf, 400, 300);

    gfx_rect_t below = { 0, 21, 400, 279 };

    s_lora.active_channel_count = 1;
    snprintf(s_lora.channel_name, sizeof(s_lora.channel_name), "#longchannel");
    s_lora.client_repeat = true;
    s_lora.packet_count = 123;
    s_lora.packets_sent = 45;
    s_lora.forwarded_count = 12;
    s_lora.last_rssi_dbm = -115;
    s_lora.last_snr_q4 = -50;
    s_lora.noise_floor_dbm = -105;

    gfx_clear(&s_fb, GFX_WHITE);
    ui_draw_lora_view(&s_fb, below, &s_ctx);

    const char *compact_right = "RSSI -115 \xC2\xB7 -12.5 dB \xC2\xB7 NF -105";
    int right_w = gfx_text_width(&gfx_font_sans_12, compact_right);
    int right_box_w = right_w + 2;
    if (right_box_w > 240) right_box_w = 240;
    int right_x = 392 - right_box_w;

    memset(expected_buf, 0, sizeof(expected_buf));
    gfx_clear(&expected_fb, GFX_WHITE);
    gfx_text_in_rect(&expected_fb, &gfx_font_sans_12, (gfx_rect_t){ right_x, 40, right_box_w, 16 },
                     GFX_ALIGN_RIGHT, compact_right, GFX_BLACK);
    TEST_ASSERT_TRUE(rect_pixels_equal(&s_fb, &expected_fb, right_x, 40, 392, 56));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ui_lora_disabled);
    RUN_TEST(test_ui_lora_no_messages);
    RUN_TEST(test_ui_lora_multiline_message_wrapping);
    RUN_TEST(test_ui_lora_unbroken_long_word_wrapping);
    RUN_TEST(test_ui_lora_chronological_scrolling_keeps_newest);
    RUN_TEST(test_ui_lora_negative_fractional_snr_and_zero_pin);
    RUN_TEST(test_ui_status_draws_lora_and_ble_indicator_when_enabled);
    RUN_TEST(test_ui_lora_dm_outgoing_ack_and_multichannel_badges);
    RUN_TEST(test_ui_lora_hard_newlines_break_lines_without_missing_glyph_box);
    RUN_TEST(test_ui_lora_noise_floor_zero_omitted);
    RUN_TEST(test_ui_lora_channel_name_formatting);
    RUN_TEST(test_ui_lora_header_line2_channel_all_and_repeat_off);
    RUN_TEST(test_ui_lora_header_line2_no_overlap_on_long_rssi);
    return UNITY_END();
}
