#include "radar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DWD OpenData WMS (Deutscher Wetterdienst): 1x1 km Radar_rv over central Europe,
 * ICON-EU precipitation across Europe. */

#define RV_WIDTH_MAX_M 1600000.0
#define STEP_VIEW_W 400
#define STEP_VIEW_H 279

static int chan_diff(uint8_t a, uint8_t b)
{
    return a > b ? (int)(a - b) : (int)(b - a);
}

static bool same_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t tr, uint8_t tg, uint8_t tb)
{
    return chan_diff(r, tr) <= 4 && chan_diff(g, tg) <= 4 && chan_diff(b, tb) <= 4;
}

static radar_level_t rv_level_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 128) {
        return RADAR_NONE; /* transparent or semi-transparent "Keine Daten" (126,126,126,77) */
    }
    if (r >= 230 && g <= 40 && b >= 230) {
        return RADAR_NONE; /* magenta radar range perimeter (251,0,255) */
    }
    if (r == 126 && g == 126 && b == 126) {
        return RADAR_NONE; /* opaque grey "Keine Daten" */
    }

    /* Official DWD Radar_rv legend swatches [0.1..2.0) mm/h -> LIGHT */
    if (same_rgb(r, g, b, 51, 255, 255) || same_rgb(r, g, b, 26, 204, 154) ||
        same_rgb(r, g, b, 1, 153, 52) || same_rgb(r, g, b, 77, 179, 27)) {
        return RADAR_LIGHT;
    }
    /* [2.0..10.0) mm/h -> MODERATE */
    if (same_rgb(r, g, b, 153, 204, 1) || same_rgb(r, g, b, 204, 230, 1) ||
        same_rgb(r, g, b, 255, 255, 1) || same_rgb(r, g, b, 255, 196, 1)) {
        return RADAR_MODERATE;
    }
    /* >= 10.0 mm/h -> HEAVY */
    if (same_rgb(r, g, b, 255, 137, 1) || same_rgb(r, g, b, 255, 69, 1) ||
        same_rgb(r, g, b, 254, 0, 0) || same_rgb(r, g, b, 229, 0, 76) ||
        same_rgb(r, g, b, 204, 0, 152) || same_rgb(r, g, b, 102, 0, 203) ||
        same_rgb(r, g, b, 0, 0, 254)) {
        return RADAR_HEAVY;
    }

    /* Fallback RGB classification if GeoServer blends an unknown shade, excluding greys & purple. */
    if (chan_diff(r, g) < 15 && chan_diff(g, b) < 15) {
        return RADAR_NONE;
    }
    if (r > 200 && b > 200 && g < 60) {
        return RADAR_NONE;
    }
    if ((r >= 240 && g <= 155) || (b >= 220 && r <= 20 && g <= 20) || (b >= 130 && r >= 90 && g <= 60)) {
        return RADAR_HEAVY;
    }
    if (r >= 145 && g >= 180 && b <= 60) {
        return RADAR_MODERATE;
    }
    return RADAR_LIGHT;
}

static radar_level_t iconeu_level_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 128) {
        return RADAR_NONE;
    }
    /* ICON-EU [0.1..2.0) mm/1h swatch is solid silver-grey (192,192,192,255). */
    if (same_rgb(r, g, b, 192, 192, 192)) {
        return RADAR_LIGHT;
    }
    /* [2.0..10.0) mm/1h swatches are pale/bright yellows (254,254,192) and (254,254,91). */
    if (same_rgb(r, g, b, 254, 254, 192) || same_rgb(r, g, b, 254, 254, 91)) {
        return RADAR_MODERATE;
    }
    /* Exclude pure transparent-white or neutral non-swatch greys outside the legend. */
    if (same_rgb(r, g, b, 255, 255, 255) || same_rgb(r, g, b, 0, 0, 0)) {
        return RADAR_NONE;
    }
    /* Remaining palette entries (>= 10 mm/1h): lime, greens, cyan, blue, purples, reds, dark grey. */
    return RADAR_HEAVY;
}

bool radar_dwd_covers(double lat, double lon)
{
    return lat >= 29.5 && lat <= 70.5 && lon >= -23.5 && lon <= 62.5;
}

radar_dwd_layer_t radar_dwd_layer_for_view(const map_view_t *view)
{
    map_bounds_t b;
    map_view_bounds(view, &b);
    double width_m = map_metres_per_px(view) * (double)view->w;
    if (b.lat_min >= 45.8 && b.lat_max <= 56.1 && b.lon_min >= 1.6 && b.lon_max <= 18.6 &&
        width_m <= RV_WIDTH_MAX_M) {
        return RADAR_DWD_LAYER_RV;
    }
    return RADAR_DWD_LAYER_ICONEU;
}

uint32_t radar_dwd_step_s(double lat, double lon, double zoom)
{
    map_view_t v;
    map_view_init(&v, (int32_t)lround(lat * 1e4), (int32_t)lround(lon * 1e4), zoom, STEP_VIEW_W,
                  STEP_VIEW_H);
    return radar_dwd_layer_for_view(&v) == RADAR_DWD_LAYER_RV ? RADAR_DWD_RV_STEP_S
                                                             : RADAR_DWD_ICONEU_STEP_S;
}

time_t radar_dwd_newest(time_t now, uint32_t step_s)
{
    if (step_s == 0) {
        step_s = RADAR_DWD_RV_STEP_S;
    }
    time_t delay = (step_s == RADAR_DWD_ICONEU_STEP_S) ? 0 : (time_t)RADAR_DWD_RV_STEP_S;
    if (now <= delay) {
        return 0;
    }
    return ((now - delay) / (time_t)step_s) * (time_t)step_s;
}

void radar_dwd_url(const map_view_t *view, radar_dwd_layer_t layer, time_t step_time, char *out,
                   size_t size)
{
    double minx, miny, maxx, maxy;
    map_pixel_to_mercator(view, 0, view->h, &minx, &miny);
    map_pixel_to_mercator(view, view->w, 0, &maxx, &maxy);
    const char *name = (layer == RADAR_DWD_LAYER_RV) ? RADAR_DWD_RV_LAYER : RADAR_DWD_ICONEU_LAYER;
    const char *opts = (layer == RADAR_DWD_LAYER_RV) ? "&FORMAT_OPTIONS=antialias:none" : "";

    char time_q[80] = "";
    if (step_time != 0) {
        struct tm tm;
        gmtime_r(&step_time, &tm);
        snprintf(time_q, sizeof(time_q), "&TIME=%04d-%02d-%02dT%02d:%02d:00Z", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }

    snprintf(out, size,
             "%s?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=%s&CRS=EPSG:3857"
             "&BBOX=%.2f,%.2f,%.2f,%.2f&WIDTH=%d&HEIGHT=%d&FORMAT=image/png&TRANSPARENT=TRUE%s%s",
             RADAR_DWD_WMS_URL, name, minx, miny, maxx, maxy, view->w, view->h, opts, time_q);
}

typedef struct {
    radar_frame_t *out;
    radar_dwd_layer_t layer;
    uint8_t lut[256];
    int lut_ready;
    bool type_ok;
} decode_ctx_t;

static void build_lut(decode_ctx_t *c, const png_info_t *info)
{
    if (c->lut_ready) {
        return;
    }
    memset(c->lut, 0, sizeof(c->lut));
    for (int i = 0; i < info->palette_size; i++) {
        c->lut[i] = (uint8_t)iconeu_level_rgba(info->palette[i][0], info->palette[i][1],
                                               info->palette[i][2], info->palette[i][3]);
    }
    c->lut_ready = 1;
}

static void on_row(void *ctx, int y, const uint8_t *row, const png_info_t *info)
{
    decode_ctx_t *c = ctx;
    if (c->layer == RADAR_DWD_LAYER_RV) {
        if (info->type != PNG_RGBA) {
            c->type_ok = false;
            return;
        }
        for (int x = 0; x < info->width; x++) {
            const uint8_t *px = row + 4 * x;
            radar_level_t lvl = rv_level_rgba(px[0], px[1], px[2], px[3]);
            if (lvl != RADAR_NONE) {
                radar_frame_set(c->out, x, y, lvl);
            }
        }
        return;
    }

    /* ICON-EU returns 4-bit palette PNG (or RGBA if ever unpaletted). */
    if (info->type == PNG_PALETTE) {
        build_lut(c, info);
        for (int x = 0; x < info->width; x++) {
            radar_level_t lvl = (radar_level_t)c->lut[row[x]];
            if (lvl != RADAR_NONE) {
                radar_frame_set(c->out, x, y, lvl);
            }
        }
    } else if (info->type == PNG_RGBA) {
        for (int x = 0; x < info->width; x++) {
            const uint8_t *px = row + 4 * x;
            radar_level_t lvl = iconeu_level_rgba(px[0], px[1], px[2], px[3]);
            if (lvl != RADAR_NONE) {
                radar_frame_set(c->out, x, y, lvl);
            }
        }
    } else {
        c->type_ok = false;
    }
}

png_err_t radar_dwd_decode(const uint8_t *png, size_t len, const png_mem_t *mem,
                           const map_view_t *view, radar_dwd_layer_t layer, uint32_t frame_time,
                           radar_frame_t *out)
{
    if (png == NULL || view == NULL || out == NULL || view->w <= 0 || view->h <= 0) {
        return PNG_ERR_FORMAT;
    }
    if (!radar_frame_alloc(out, (uint16_t)view->w, (uint16_t)view->h, mem)) {
        return PNG_ERR_NO_MEMORY;
    }

    double minx, miny, maxx, maxy;
    map_pixel_to_mercator(view, 0, view->h, &minx, &miny);
    map_pixel_to_mercator(view, view->w, 0, &maxx, &maxy);
    out->time = frame_time;
    out->source = RADAR_SOURCE_DWD;
    out->mx0 = minx;
    out->my0 = maxy;
    /* Web Mercator metres per pixel (matches radar_frame.c view_metres and radar_rainviewer.c). */
    out->scale = (maxx - minx) / (double)view->w;

    static png_info_t info; /* 1 KB; off the sync task's stack */
    decode_ctx_t ctx = { .out = out, .layer = layer, .type_ok = true };
    png_err_t err = png_decode(png, len, mem, &info, on_row, &ctx);
    if (err != PNG_OK) {
        radar_frame_free(out, mem);
        return err;
    }
    if (info.width != view->w || info.height != view->h) {
        radar_frame_free(out, mem);
        return PNG_ERR_FORMAT;
    }
    if (!ctx.type_ok) {
        radar_frame_free(out, mem);
        return PNG_ERR_UNSUPPORTED;
    }
    return PNG_OK;
}
