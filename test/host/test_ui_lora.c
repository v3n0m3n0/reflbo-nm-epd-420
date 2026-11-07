#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include "context_fixtures.h"
#include "gfx.h"
#include "lang.h"
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ui_lora_disabled);
    RUN_TEST(test_ui_lora_no_messages);
    RUN_TEST(test_ui_lora_multiline_message_wrapping);
    RUN_TEST(test_ui_lora_unbroken_long_word_wrapping);
    RUN_TEST(test_ui_lora_chronological_scrolling_keeps_newest);
    return UNITY_END();
}
