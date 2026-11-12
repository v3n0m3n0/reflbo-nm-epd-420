#include "ui_lora.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_fonts.h"
#include "lang.h"

#define LORA_MSG_MAX_LINES   3
#define LORA_MSG_LINE_W      384
#define LORA_MSG_LINE_H      13
#define LORA_MSG_LINE_BUF_SZ 160

static void format_bw(uint32_t bw_hz, char *out, size_t size)
{
    if (bw_hz % 1000 == 0) {
        snprintf(out, size, "%lu", (unsigned long)(bw_hz / 1000));
    } else {
        snprintf(out, size, "%lu.%lu", (unsigned long)(bw_hz / 1000), (unsigned long)((bw_hz % 1000) / 100));
    }
}

static void format_snr(int8_t snr_q4, char *out, size_t size)
{
    int whole = snr_q4 / 4;
    int frac = abs(snr_q4 % 4) * 25 / 10;
    snprintf(out, size, "%+d.%d dB", whole, frac);
}

static void format_lora_msg(const ui_lora_msg_t *msg, char *out, size_t out_sz)
{
    char time_str[16];
    if (msg->timestamp >= 1577836800u) {
        time_t t = (time_t)msg->timestamp;
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(time_str, sizeof(time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(time_str, sizeof(time_str), "%s", msg->outgoing ? "[TX]" : "[RX]");
    }

    if (msg->outgoing) {
        if (msg->sender[0] != '\0') {
            snprintf(out, out_sz, "%s > %s: %s",
                     time_str, msg->sender, msg->text);
        } else {
            snprintf(out, out_sz, "%s > %s",
                     time_str, msg->text);
        }
    } else {
        const char *dir = msg->is_direct ? "< [DM]" : "<";
        if (msg->sender[0] != '\0') {
            snprintf(out, out_sz, "%s %s %s: %s",
                     time_str, dir, msg->sender, msg->text);
        } else {
            snprintf(out, out_sz, "%s %s %s",
                     time_str, dir, msg->text);
        }
    }
}

static int wrap_lora_msg_lines(const gfx_font_t *font, const char *text, int16_t max_w,
                               char lines[LORA_MSG_MAX_LINES][LORA_MSG_LINE_BUF_SZ])
{
    if (lines == NULL) {
        return 0;
    }
    for (int i = 0; i < LORA_MSG_MAX_LINES; i++) {
        lines[i][0] = '\0';
    }
    if (font == NULL || text == NULL || text[0] == '\0') {
        return 1;
    }

    while (*text == ' ' || *text == '\n' || *text == '\r') {
        text++;
    }
    if (*text == '\0') {
        return 1;
    }

    for (int line_idx = 0; line_idx < LORA_MSG_MAX_LINES; line_idx++) {
        if (line_idx > 0) {
            while (*text == ' ' || *text == '\n' || *text == '\r') {
                text++;
            }
            if (*text == '\0') {
                return line_idx;
            }
        }

        if (line_idx == LORA_MSG_MAX_LINES - 1) {
            if (gfx_text_width(font, text) <= max_w) {
                snprintf(lines[line_idx], LORA_MSG_LINE_BUF_SZ, "%.*s", LORA_MSG_LINE_BUF_SZ - 1, text);
            } else {
                gfx_text_ellipsize(font, text, max_w, lines[line_idx], LORA_MSG_LINE_BUF_SZ);
            }
            return LORA_MSG_MAX_LINES;
        }

        if (gfx_text_width(font, text) <= max_w) {
            snprintf(lines[line_idx], LORA_MSG_LINE_BUF_SZ, "%.*s", LORA_MSG_LINE_BUF_SZ - 1, text);
            return line_idx + 1;
        }

        const char *p = text;
        const char *last_char_p = text;
        const char *last_space_p = NULL;
        const char *next_after_space_p = NULL;

        while (*p != '\0') {
            const char *prev = p;
            gfx_utf8_next(&p);
            size_t len = (size_t)(p - text);
            if (len >= LORA_MSG_LINE_BUF_SZ) {
                break;
            }
            char candidate[LORA_MSG_LINE_BUF_SZ];
            memcpy(candidate, text, len);
            candidate[len] = '\0';
            if (gfx_text_width(font, candidate) > max_w) {
                break;
            }
            last_char_p = p;
            if (*prev == ' ' || *prev == '\n' || *prev == '\r') {
                last_space_p = prev;
                next_after_space_p = p;
            }
        }

        size_t break_len = 0;
        const char *next_text = NULL;

        if (last_space_p != NULL && last_space_p > text) {
            break_len = (size_t)(last_space_p - text);
            next_text = next_after_space_p;
        } else {
            /* If a single word has no spaces and exceeds max_w, break at last complete UTF-8 character boundary */
            if (last_char_p == text) {
                p = text;
                gfx_utf8_next(&p);
                last_char_p = p;
            }
            break_len = (size_t)(last_char_p - text);
            next_text = last_char_p;
        }

        if (break_len >= LORA_MSG_LINE_BUF_SZ) {
            break_len = LORA_MSG_LINE_BUF_SZ - 1;
        }
        memcpy(lines[line_idx], text, break_len);
        lines[line_idx][break_len] = '\0';

        /* Trim trailing whitespace from line */
        while (break_len > 0 && (lines[line_idx][break_len - 1] == ' ' ||
                                 lines[line_idx][break_len - 1] == '\r' ||
                                 lines[line_idx][break_len - 1] == '\n')) {
            lines[line_idx][--break_len] = '\0';
        }

        text = next_text;
    }

    return LORA_MSG_MAX_LINES;
}

typedef struct {
    int line_count;
    char lines[LORA_MSG_MAX_LINES][LORA_MSG_LINE_BUF_SZ];
} wrapped_lora_msg_t;

void ui_draw_lora_view(gfx_fb_t *fb, gfx_rect_t below, const ui_context_t *ctx)
{
    if (fb == NULL || ctx == NULL) {
        return;
    }
    const ui_lora_t *lora = ctx->lora;
    if (lora == NULL || !lora->enabled) {
        gfx_text_in_rect(fb, &gfx_font_sans_16, below, GFX_ALIGN_CENTER,
                         lang_str(ctx->lang, LS_T_LORA_OFF), GFX_BLACK);
        return;
    }

    const gfx_font_t *font_sm = &gfx_font_sans_12;
    const gfx_font_t *font_md = &gfx_font_sans_16;

    /* --- 1. Header Strip (y = 21..58) --- */
    char left_str[128];
    char right_str[64];
    char bw_str[16];
    format_bw(lora->bw_hz, bw_str, sizeof(bw_str));

    const char *name = lora->node_name[0] ? lora->node_name : "NM-EPD-420";
    if (lora->freq_khz % 1000 == 0) {
        snprintf(left_str, sizeof(left_str), "%s \xC2\xB7 %lu MHz \xC2\xB7 SF%u \xC2\xB7 BW%s",
                 name, (unsigned long)(lora->freq_khz / 1000), lora->sf, bw_str);
    } else {
        snprintf(left_str, sizeof(left_str), "%s \xC2\xB7 %lu.%03lu MHz \xC2\xB7 SF%u \xC2\xB7 BW%s",
                 name, (unsigned long)(lora->freq_khz / 1000), (unsigned long)(lora->freq_khz % 1000),
                 lora->sf, bw_str);
    }

    if (lora->ble_pin > 0) {
        if (lora->ble_connected) {
            snprintf(right_str, sizeof(right_str), "PIN: %06lu [BLE]", (unsigned long)lora->ble_pin);
        } else {
            snprintf(right_str, sizeof(right_str), "PIN: %06lu", (unsigned long)lora->ble_pin);
        }
    } else {
        snprintf(right_str, sizeof(right_str), "PIN: \xE2\x80\x94");
    }

    gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, 23, 260, 16 }, GFX_ALIGN_LEFT, left_str, GFX_BLACK);
    gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 268, 23, 124, 16 }, GFX_ALIGN_RIGHT, right_str, GFX_BLACK);

    /* Line 2 (y = 41) */
    const char *ch = lora->channel_name[0] ? lora->channel_name : "Public";
    snprintf(left_str, sizeof(left_str), "Ch: %s \xC2\xB7 RX: %lu \xC2\xB7 TX: %lu \xC2\xB7 Fwd: %lu",
             ch, (unsigned long)lora->packet_count, (unsigned long)lora->packets_sent,
             (unsigned long)lora->sent_flood);

    if (lora->packet_count > 0) {
        char snr_str[16];
        format_snr(lora->last_snr_q4, snr_str, sizeof(snr_str));
        snprintf(right_str, sizeof(right_str), "RSSI %d dBm \xC2\xB7 %s \xC2\xB7 NF %d",
                 lora->last_rssi_dbm, snr_str, lora->noise_floor_dbm);
    } else {
        snprintf(right_str, sizeof(right_str), "TX %+d dBm \xC2\xB7 NF %d dBm",
                 lora->tx_power_dbm, lora->noise_floor_dbm);
    }

    gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, 40, 240, 16 }, GFX_ALIGN_LEFT, left_str, GFX_BLACK);
    gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 248, 40, 144, 16 }, GFX_ALIGN_RIGHT, right_str, GFX_BLACK);

    /* Horizontal separator at y = 58 */
    gfx_hline(fb, 8, 58, 384, GFX_BLACK);

    /* --- 2. Middle Section — Received & Sent Messages (y = 59..208) --- */
    gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, 61, 250, 16 }, GFX_ALIGN_LEFT,
                     lang_str(ctx->lang, LS_LORA_MESSAGES), GFX_BLACK);
    int display_msg_count = lora->total_msg_count > 0 ? lora->total_msg_count : lora->msg_count;
    if (display_msg_count > 0) {
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "(%d)", display_msg_count);
        gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 258, 61, 134, 16 }, GFX_ALIGN_RIGHT, count_str, GFX_BLACK);
    }

    if (lora->msg_count == 0) {
        gfx_text_in_rect(fb, font_md, (gfx_rect_t){ 8, 105, 384, 20 }, GFX_ALIGN_CENTER,
                         lang_str(ctx->lang, LS_LORA_NO_MESSAGES), GFX_BLACK);
        char hint[80];
        snprintf(hint, sizeof(hint), "BLE: MeshCore-%s \xC2\xB7 PIN: %06lu",
                 name, (unsigned long)lora->ble_pin);
        gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, 133, 384, 16 }, GFX_ALIGN_CENTER, hint, GFX_BLACK);
        gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, 153, 384, 16 }, GFX_ALIGN_CENTER,
                         "BOOT short: send Self Advert", GFX_BLACK);
    } else {
        int count = lora->msg_count > UI_LORA_MAX_MSGS ? UI_LORA_MAX_MSGS : lora->msg_count;
        static wrapped_lora_msg_t wrapped[UI_LORA_MAX_MSGS];
        int total_h = 0;
        int first_visible = count - 1;

        for (int i = count - 1; i >= 0; i--) {
            char full_msg[256];
            format_lora_msg(&lora->msgs[i], full_msg, sizeof(full_msg));
            wrapped[i].line_count = wrap_lora_msg_lines(font_sm, full_msg, LORA_MSG_LINE_W, wrapped[i].lines);
            int msg_h = wrapped[i].line_count * LORA_MSG_LINE_H + 3;
            if (i == count - 1) {
                total_h += msg_h;
                first_visible = i;
            } else {
                if (total_h + msg_h <= (205 - 76)) {
                    total_h += msg_h;
                    first_visible = i;
                } else {
                    break;
                }
            }
        }

        int y_pos = 76;
        for (int i = first_visible; i < count; i++) {
            for (int l = 0; l < wrapped[i].line_count; l++) {
                gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, (int16_t)y_pos, LORA_MSG_LINE_W, 14 },
                                 GFX_ALIGN_LEFT, wrapped[i].lines[l], GFX_BLACK);
                y_pos += LORA_MSG_LINE_H;
            }
            y_pos += 3;
        }
    }

    /* Horizontal separator at y = 208 */
    gfx_hline(fb, 8, 208, 384, GFX_BLACK);

    /* --- 3. Bottom Section — Heard Nodes / Neighbors (y = 209..299) --- */
    gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, 211, 250, 16 }, GFX_ALIGN_LEFT,
                     lang_str(ctx->lang, LS_LORA_NODES), GFX_BLACK);
    int display_node_count = lora->total_node_count > 0 ? lora->total_node_count : lora->node_count;
    if (display_node_count > 0) {
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "(%d)", display_node_count);
        gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 258, 211, 134, 16 }, GFX_ALIGN_RIGHT, count_str, GFX_BLACK);
    }

    if (lora->node_count == 0) {
        gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, 245, 384, 16 }, GFX_ALIGN_CENTER,
                         lang_str(ctx->lang, LS_LORA_NO_NODES), GFX_BLACK);
    } else {
        int max_nodes = lora->node_count > 3 ? 3 : lora->node_count;
        int y_pos = 230;
        for (int i = 0; i < max_nodes; i++) {
            const ui_lora_node_t *node = &lora->nodes[i];
            char node_id[32];
            if (node->name[0] != '\0') {
                snprintf(node_id, sizeof(node_id), "%s", node->name);
            } else {
                snprintf(node_id, sizeof(node_id), "%02X%02X%02X%02X",
                         node->pub_key_prefix[0], node->pub_key_prefix[1],
                         node->pub_key_prefix[2], node->pub_key_prefix[3]);
            }

            const char *badge = "";
            switch (node->node_type) {
            case 1: badge = "[Chat] "; break;
            case 2: badge = "[Rep] "; break;
            case 3: badge = "[Room] "; break;
            case 4: badge = "[Sens] "; break;
            default: break;
            }

            uint32_t age_min = 0;
            if (ctx->now >= (time_t)node->last_seen && node->last_seen > 0) {
                age_min = (uint32_t)(ctx->now - (time_t)node->last_seen) / 60;
            }

            char snr_buf[16];
            format_snr(node->last_snr_q4, snr_buf, sizeof(snr_buf));

            char node_row[160];
            snprintf(node_row, sizeof(node_row), "%s%s \xC2\xB7 %d dBm \xC2\xB7 %s \xC2\xB7 h:%u \xC2\xB7 %lumi",
                     badge, node_id, node->last_rssi_dbm, snr_buf,
                     (unsigned)node->hops, (unsigned long)age_min);

            char fit[160];
            gfx_text_ellipsize(font_sm, node_row, 384, fit, sizeof(fit));
            gfx_text_in_rect(fb, font_sm, (gfx_rect_t){ 8, (int16_t)y_pos, 384, 16 },
                             GFX_ALIGN_LEFT, fit, GFX_BLACK);
            y_pos += 22;
        }
    }
}
