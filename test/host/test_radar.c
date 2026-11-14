#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "radar.h"
#include "unity.h"

/* The weather radar (spec §11.2). The level counts come from DWD WMS (Radar_rv 1x1 km RGBA
 * and ICON-EU 4-bit palette) and RainViewer's Universal Blue / < 20 dBZ RGBA table. */

void setUp(void) {}
void tearDown(void) {}

static uint8_t *load(const char *name, size_t *len)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*len + 1);
    TEST_ASSERT_EQUAL_size_t(*len, fread(data, 1, *len, f));
    data[*len] = '\0';
    fclose(f);
    return data;
}

static void count_levels(const radar_frame_t *f, long out[4])
{
    memset(out, 0, 4 * sizeof(long));
    for (int y = 0; y < f->h; y++) {
        for (int x = 0; x < f->w; x++) {
            out[radar_frame_level(f, x, y)]++;
        }
    }
}

#define T_20260924_1120 1790248800 /* 2026-09-24 11:20 UTC */

static uint32_t test_crc32(uint32_t crc, const uint8_t *buf, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static size_t put_png_chunk(uint8_t *out, const char type[4], const uint8_t *body, uint32_t n)
{
    out[0] = (uint8_t)(n >> 24);
    out[1] = (uint8_t)(n >> 16);
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)n;
    memcpy(out + 4, type, 4);
    if (n > 0) {
        memcpy(out + 8, body, n);
    }
    uint32_t crc = test_crc32(test_crc32(0, (const uint8_t *)type, 4), body, n);
    out[8 + n] = (uint8_t)(crc >> 24);
    out[9 + n] = (uint8_t)(crc >> 16);
    out[10 + n] = (uint8_t)(crc >> 8);
    out[11 + n] = (uint8_t)crc;
    return 12 + n;
}

/* Minimal valid uncompressed 8-bit RGBA PNG (w x 1 row) for unit-testing pixel decoders. */
static size_t make_rgba_row_png(uint8_t *out, uint16_t w, const uint8_t *rgba)
{
    static const uint8_t k_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    memcpy(out, k_sig, 8);
    size_t n = 8;
    uint8_t ihdr[13] = { 0, 0, (uint8_t)(w >> 8), (uint8_t)w, 0, 0, 0, 1, 8, PNG_RGBA, 0, 0, 0 };
    n += put_png_chunk(out + n, "IHDR", ihdr, 13);

    uint32_t raw_len = 1 + (uint32_t)w * 4;
    uint8_t raw[256];
    raw[0] = 0; /* filter None */
    memcpy(raw + 1, rgba, (size_t)w * 4);
    uint32_t s1 = 1, s2 = 0;
    for (uint32_t i = 0; i < raw_len; i++) {
        s1 = (s1 + raw[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    uint32_t adler = (s2 << 16) | s1;

    uint8_t idat[300];
    idat[0] = 0x78;
    idat[1] = 0x01;
    idat[2] = 0x01; /* BFINAL=1, BTYPE=00 (stored) */
    idat[3] = (uint8_t)raw_len;
    idat[4] = (uint8_t)(raw_len >> 8);
    idat[5] = (uint8_t)~idat[3];
    idat[6] = (uint8_t)~idat[4];
    memcpy(idat + 7, raw, raw_len);
    size_t zlen = 7 + raw_len;
    idat[zlen++] = (uint8_t)(adler >> 24);
    idat[zlen++] = (uint8_t)(adler >> 16);
    idat[zlen++] = (uint8_t)(adler >> 8);
    idat[zlen++] = (uint8_t)adler;

    n += put_png_chunk(out + n, "IDAT", idat, (uint32_t)zlen);
    n += put_png_chunk(out + n, "IEND", NULL, 0);
    return n;
}

static void test_dwd_covers_layers_steps_and_urls(void)
{
    TEST_ASSERT_TRUE(radar_dwd_covers(50.0755, 14.4378));  /* Prague */
    TEST_ASSERT_TRUE(radar_dwd_covers(52.2297, 21.0122));  /* Warsaw */
    TEST_ASSERT_TRUE(radar_dwd_covers(40.4168, -3.7038));  /* Madrid */
    TEST_ASSERT_TRUE(radar_dwd_covers(60.1699, 24.9384));  /* Helsinki */
    TEST_ASSERT_FALSE(radar_dwd_covers(40.7128, -74.0060)); /* New York */
    TEST_ASSERT_FALSE(radar_dwd_covers(35.6762, 139.6503)); /* Tokyo */

    map_view_t prague, warsaw, wide;
    map_view_init(&prague, 500755, 144378, 6.5, 400, 279);
    TEST_ASSERT_EQUAL_INT(RADAR_DWD_LAYER_RV, radar_dwd_layer_for_view(&prague));
    TEST_ASSERT_EQUAL_UINT32(300, radar_dwd_step_s(50.0755, 14.4378, 6.5));
    TEST_ASSERT_EQUAL_UINT32(300, radar_dwd_step_s(52.4064, 16.9252, 8.0)); /* Poznan zoomed in */

    map_view_init(&warsaw, 522297, 210122, 6.5, 400, 279);
    TEST_ASSERT_EQUAL_INT(RADAR_DWD_LAYER_ICONEU, radar_dwd_layer_for_view(&warsaw));
    TEST_ASSERT_EQUAL_UINT32(3600, radar_dwd_step_s(52.2297, 21.0122, 6.5));

    map_view_init(&wide, 500755, 144378, 3.5, 400, 279); /* > 1600 km across */
    TEST_ASSERT_EQUAL_INT(RADAR_DWD_LAYER_ICONEU, radar_dwd_layer_for_view(&wide));
    TEST_ASSERT_EQUAL_UINT32(3600, radar_dwd_step_s(50.0755, 14.4378, 3.5));

    /* Newest step rounding: 300 s delay for RV, current full hour for ICON-EU */
    TEST_ASSERT_EQUAL_INT64(T_20260924_1120, radar_dwd_newest(T_20260924_1120 + 7 * 60 + 30, 300));
    TEST_ASSERT_EQUAL_INT64(T_20260924_1120, radar_dwd_newest(T_20260924_1120 + 5 * 60, 300));
    TEST_ASSERT_EQUAL_INT64(T_20260924_1120 - 300, radar_dwd_newest(T_20260924_1120 + 4 * 60 + 59, 300));
    TEST_ASSERT_EQUAL_INT64(1790247600, radar_dwd_newest(T_20260924_1120 + 1800, 3600)); /* 11:00 */

    char url[384];
    radar_dwd_url(&prague, RADAR_DWD_LAYER_RV, T_20260924_1120, url, sizeof(url));
    TEST_ASSERT_TRUE(strlen(url) < sizeof(url) - 1);
    TEST_ASSERT_NOT_NULL(strstr(url, RADAR_DWD_RV_LAYER));
    TEST_ASSERT_NOT_NULL(strstr(url, "CRS=EPSG:3857"));
    TEST_ASSERT_NOT_NULL(strstr(url, "WIDTH=400&HEIGHT=279"));
    TEST_ASSERT_NOT_NULL(strstr(url, "FORMAT_OPTIONS=antialias:none"));
    TEST_ASSERT_NOT_NULL(strstr(url, "TIME=2026-09-24T11:20:00Z"));

    radar_dwd_url(&warsaw, RADAR_DWD_LAYER_ICONEU, 0, url, sizeof(url));
    TEST_ASSERT_NOT_NULL(strstr(url, RADAR_DWD_ICONEU_LAYER));
    TEST_ASSERT_NULL(strstr(url, "FORMAT_OPTIONS"));
    TEST_ASSERT_NULL(strstr(url, "TIME="));
}

static void test_dwd_frames_decode_rv_and_iconeu(void)
{
    size_t len;
    uint8_t *png = load("png/dwd_rv_rain.png", &len);
    map_view_t v;
    map_view_init(&v, 499000, 153000, 5.5, 400, 279);
    radar_frame_t f;
    TEST_ASSERT_EQUAL_INT(PNG_OK,
                          radar_dwd_decode(png, len, NULL, &v, RADAR_DWD_LAYER_RV, T_20260924_1120, &f));
    TEST_ASSERT_EQUAL_UINT16(400, f.w);
    TEST_ASSERT_EQUAL_UINT16(279, f.h);
    TEST_ASSERT_EQUAL_UINT8(RADAR_SOURCE_DWD, f.source);
    TEST_ASSERT_EQUAL_UINT32(T_20260924_1120, f.time);
    long n[4];
    count_levels(&f, n);
    TEST_ASSERT_EQUAL_INT32(98054, n[RADAR_NONE]);
    TEST_ASSERT_EQUAL_INT32(13477, n[RADAR_LIGHT]);
    TEST_ASSERT_EQUAL_INT32(69, n[RADAR_MODERATE]);
    TEST_ASSERT_EQUAL_INT32(0, n[RADAR_HEAVY]);
    TEST_ASSERT_EQUAL_INT(RADAR_NONE, radar_frame_level(&f, 225, 0));  /* magenta perimeter (251,0,255) */
    TEST_ASSERT_EQUAL_INT(RADAR_LIGHT, radar_frame_level(&f, 76, 21)); /* cyan rain (51,255,255) */
    radar_frame_free(&f, NULL);
    free(png);

    /* ICON-EU 4-bit palette including (192,192,192,255) -> RADAR_LIGHT */
    png = load("png/dwd_iconeu_rain.png", &len);
    map_view_init(&v, 500755, 144378, 5.0, 400, 279);
    TEST_ASSERT_EQUAL_INT(
        PNG_OK, radar_dwd_decode(png, len, NULL, &v, RADAR_DWD_LAYER_ICONEU, T_20260924_1120, &f));
    count_levels(&f, n);
    TEST_ASSERT_EQUAL_INT32(104678, n[RADAR_NONE]);
    TEST_ASSERT_EQUAL_INT32(6871, n[RADAR_LIGHT]);
    TEST_ASSERT_EQUAL_INT32(51, n[RADAR_MODERATE]);
    TEST_ASSERT_EQUAL_INT32(0, n[RADAR_HEAVY]);
    radar_frame_free(&f, NULL);
    free(png);

    /* Synthetic RV swatches: perimeter, nodata, light, moderate, heavy + size check */
    static const uint8_t k_rgba[8 * 4] = {
        251, 0,   255, 255, /* magenta range perimeter -> NONE */
        126, 126, 126, 77,  /* semi-transparent nodata -> NONE */
        51,  255, 255, 255, /* LIGHT */
        1,   153, 52,  255, /* LIGHT */
        204, 230, 1,   255, /* MODERATE */
        255, 196, 1,   255, /* MODERATE */
        254, 0,   0,   255, /* HEAVY */
        0,   0,   254, 255, /* HEAVY */
    };
    uint8_t synth[512];
    size_t slen = make_rgba_row_png(synth, 8, k_rgba);
    map_view_t v8, v7;
    map_view_init(&v8, 500755, 144378, 6.5, 8, 1);
    TEST_ASSERT_EQUAL_INT(
        PNG_OK, radar_dwd_decode(synth, slen, NULL, &v8, RADAR_DWD_LAYER_RV, T_20260924_1120, &f));
    TEST_ASSERT_EQUAL_INT(RADAR_NONE, radar_frame_level(&f, 0, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_NONE, radar_frame_level(&f, 1, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_LIGHT, radar_frame_level(&f, 2, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_LIGHT, radar_frame_level(&f, 3, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_MODERATE, radar_frame_level(&f, 4, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_MODERATE, radar_frame_level(&f, 5, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_HEAVY, radar_frame_level(&f, 6, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_HEAVY, radar_frame_level(&f, 7, 0));
    radar_frame_free(&f, NULL);

    map_view_init(&v7, 500755, 144378, 6.5, 7, 1);
    TEST_ASSERT_EQUAL_INT(
        PNG_ERR_FORMAT, radar_dwd_decode(synth, slen, NULL, &v7, RADAR_DWD_LAYER_RV, T_20260924_1120, &f));
}

/* A frame on the (49.9 N, 15.3 E z=5.5 400x279) DWD grid with one square around a place. */
static void square_at(radar_frame_t *f, double lat, double lon, int half, radar_level_t level)
{
    map_view_t fv;
    map_view_init(&fv, 499000, 153000, 5.5, 400, 279);
    TEST_ASSERT_TRUE(radar_frame_alloc(f, (uint16_t)fv.w, (uint16_t)fv.h, NULL));
    double minx, miny, maxx, maxy;
    map_pixel_to_mercator(&fv, 0, fv.h, &minx, &miny);
    map_pixel_to_mercator(&fv, fv.w, 0, &maxx, &maxy);
    f->source = RADAR_SOURCE_DWD;
    f->mx0 = minx;
    f->my0 = maxy;
    f->scale = (maxx - minx) / (double)fv.w;
    double mx, my;
    map_mercator(lat, lon, &mx, &my);
    int gx = (int)((mx - f->mx0) / f->scale), gy = (int)((f->my0 - my) / f->scale);
    for (int y = gy - half; y <= gy + half; y++) {
        for (int x = gx - half; x <= gx + half; x++) {
            radar_frame_set(f, x, y, level);
        }
    }
}

static int ink(const gfx_fb_t *fb, gfx_rect_t r)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            n += gfx_get_pixel(fb, x, y);
        }
    }
    return n;
}

static void test_rain_lands_where_it_falls_in_any_view(void)
{
    static uint8_t buf[400 * 300 / 8];
    gfx_fb_t fb;
    gfx_fb_init(&fb, buf, 400, 300);
    for (int z = 0; z < 2; z++) {
        double zoom = z == 0 ? 6.5 : 8.0;
        static const radar_level_t k_levels[3] = { RADAR_HEAVY, RADAR_MODERATE, RADAR_LIGHT };
        static const int k_ink[3] = { 64, 32, 16 }; /* of an 8 x 8 square: solid, half, a quarter */
        for (int i = 0; i < 3; i++) {
            radar_frame_t f;
            square_at(&f, 49.1951, 16.6068, 8, k_levels[i]); /* 17 grid pixels around Brno */
            map_view_t v;
            map_view_init(&v, 491951, 166068, zoom, 400, 280);
            gfx_clear(&fb, GFX_WHITE);
            radar_render(&fb, (gfx_rect_t){ 0, 20, 400, 280 }, &v, &f);
            TEST_ASSERT_EQUAL_INT_MESSAGE(k_ink[i], ink(&fb, (gfx_rect_t){ 196, 156, 8, 8 }), "at the centre");
            TEST_ASSERT_EQUAL_INT(0, ink(&fb, (gfx_rect_t){ 0, 20, 60, 60 })); /* dry far away */
            TEST_ASSERT_TRUE(radar_any_rain(&v, &f));
            radar_frame_free(&f, NULL);
        }
    }
}

static void test_outside_the_data_the_edge_is_dotted(void)
{
    static uint8_t buf[400 * 300 / 8];
    gfx_fb_t fb;
    gfx_fb_init(&fb, buf, 400, 300);
    gfx_clear(&fb, GFX_WHITE);
    radar_frame_t f;
    square_at(&f, 49.1951, 16.6068, 2, RADAR_HEAVY);
    map_view_t v;
    map_view_init(&v, 470000, 153000, 6.5, 400, 280); /* centred just south of f's bottom edge */
    radar_render(&fb, (gfx_rect_t){ 0, 20, 400, 280 }, &v, &f);
    double x, y;
    map_project(&v, 47.026648, 15.3000, &x, &y); /* the south edge of f */
    int row = 20 + (int)lround(y);
    int dots = ink(&fb, (gfx_rect_t){ 0, (int16_t)(row - 1), 400, 3 });
    TEST_ASSERT_TRUE_MESSAGE(dots > 100 && dots < 300, "a dotted line, every other pixel");
    TEST_ASSERT_FALSE(radar_any_rain(&(map_view_t){ .cx = 0, .cy = 0, .zoom = 6.5, .w = 400, .h = 280 }, &f));
    radar_frame_free(&f, NULL);
}

static void test_the_rainviewer_index_names_its_frames(void)
{
    size_t len;
    char *json = (char *)load("radar/rainviewer_maps.json", &len);
    radar_rv_index_t idx;
    TEST_ASSERT_TRUE(radar_rv_parse_index(json, len, &idx));
    TEST_ASSERT_EQUAL_STRING("https://tilecache.rainviewer.com", idx.host);
    TEST_ASSERT_EQUAL_INT(13, idx.count);
    TEST_ASSERT_EQUAL_UINT32(1790881800, idx.frames[0].time);
    TEST_ASSERT_EQUAL_STRING("/v2/radar/c15f405b89f4", idx.frames[12].path);
    TEST_ASSERT_EQUAL_UINT32(1790889000, idx.frames[12].time);
    TEST_ASSERT_FALSE(radar_rv_parse_index("{\"radar\":{\"past\":[]}}", 21, &idx));
    TEST_ASSERT_FALSE(radar_rv_parse_index("[]", 2, &idx));
    free(json);
}

static void test_rainviewer_tiles_cover_the_view(void)
{
    map_view_t v;
    map_view_init(&v, 491951, 166068, 6.5, 400, 280);
    radar_rv_tiles_t t;
    radar_rv_tiles(&v, &t);
    TEST_ASSERT_EQUAL_INT(6, t.z);
    TEST_ASSERT_EQUAL_INT(34, t.x0);
    TEST_ASSERT_EQUAL_INT(21, t.y0);
    TEST_ASSERT_EQUAL_INT(2, t.nx);
    TEST_ASSERT_EQUAL_INT(2, t.ny);
    map_view_init(&v, 491951, 166068, 9.0, 400, 280);
    radar_rv_tiles(&v, &t);
    TEST_ASSERT_EQUAL_INT(RADAR_RV_MAX_ZOOM, t.z); /* nothing above zoom 7: enlarged */
    TEST_ASSERT_TRUE(t.nx <= 2 && t.ny <= 2);
    char url[160];
    radar_rv_tile_url(url, sizeof(url), "https://tilecache.rainviewer.com", "/v2/radar/c15f405b89f4", 7, 69, 43);
    TEST_ASSERT_EQUAL_STRING("https://tilecache.rainviewer.com/v2/radar/c15f405b89f4/256/7/69/43/2/0_0.png", url);
}

static void test_a_rainviewer_tile_decodes_by_its_colour_table(void)
{
    size_t len;
    uint8_t *png = load("png/rainviewer_tile.png", &len); /* zoom 3, tile (4, 2) */
    radar_rv_tiles_t t = { .z = 3, .x0 = 4, .y0 = 2, .nx = 1, .ny = 1 };
    radar_frame_t f;
    TEST_ASSERT_TRUE(radar_rv_frame_alloc(&f, &t, 1790889000, NULL));
    TEST_ASSERT_EQUAL_UINT16(256, f.w);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 2 * 3.14159265358979323846 * MAP_EARTH_R / (256 * 8), f.scale);
    TEST_ASSERT_EQUAL_INT(PNG_OK, radar_rv_decode_tile(png, len, NULL, &t, 4, 2, &f));
    long n[4];
    count_levels(&f, n);
    TEST_ASSERT_EQUAL_INT32(57811, n[RADAR_NONE]);
    TEST_ASSERT_EQUAL_INT32(7050, n[RADAR_LIGHT]);
    TEST_ASSERT_EQUAL_INT32(629, n[RADAR_MODERATE]);
    TEST_ASSERT_EQUAL_INT32(46, n[RADAR_HEAVY]);
    radar_frame_free(&f, NULL);
    free(png);

    /* Light < 20 dBZ echoes and semi-transparent alpha >= 80 decode cleanly; alpha < 80 stays NONE. */
    static const uint8_t k_rv_rgba[5 * 4] = {
        210, 196, 139, 160, /* < 20 dBZ tan cloud -> LIGHT */
        54,  186, 229, 255, /* intermediate cyan/blue -> LIGHT */
        210, 196, 139, 79,  /* trace alpha < 80 -> NONE */
        255, 165, 0,   200, /* orange -> MODERATE */
        230, 20,  60,  220, /* red -> HEAVY */
    };
    uint8_t rv_png[512];
    size_t rv_len = make_rgba_row_png(rv_png, 5, k_rv_rgba);
    radar_rv_tiles_t t1 = { .z = 3, .x0 = 4, .y0 = 2, .nx = 1, .ny = 1 };
    TEST_ASSERT_TRUE(radar_rv_frame_alloc(&f, &t1, 1790889000, NULL));
    TEST_ASSERT_EQUAL_INT(PNG_OK, radar_rv_decode_tile(rv_png, rv_len, NULL, &t1, 4, 2, &f));
    TEST_ASSERT_EQUAL_INT(RADAR_LIGHT, radar_frame_level(&f, 0, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_LIGHT, radar_frame_level(&f, 1, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_NONE, radar_frame_level(&f, 2, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_MODERATE, radar_frame_level(&f, 3, 0));
    TEST_ASSERT_EQUAL_INT(RADAR_HEAVY, radar_frame_level(&f, 4, 0));
    radar_frame_free(&f, NULL);
}

static void test_a_frame_covers_the_views_it_has_the_rain_for(void)
{
    map_view_t v, slot, deeper, paris;
    map_view_init(&v, 525200, 134050, 6.5, RADAR_VIEW_W, RADAR_VIEW_H); /* Berlin: RainViewer */
    radar_rv_tiles_t t;
    radar_rv_tiles(&v, &t);
    radar_frame_t f;
    TEST_ASSERT_TRUE(radar_rv_frame_alloc(&f, &t, 1790889000, NULL));
    TEST_ASSERT_TRUE(radar_frame_covers(&f, &v));
    map_view_init(&slot, 525200, 134050, 6.5, 196, 120); /* a slot's map: some of the same tiles */
    TEST_ASSERT_TRUE(radar_frame_covers(&f, &slot));
    map_view_init(&deeper, 525200, 134050, 7.0, RADAR_VIEW_W, RADAR_VIEW_H); /* zoom 7's tiles */
    TEST_ASSERT_FALSE(radar_frame_covers(&f, &deeper));
    map_view_init(&paris, 488566, 23522, 6.5, RADAR_VIEW_W, RADAR_VIEW_H);
    TEST_ASSERT_FALSE(radar_frame_covers(&f, &paris));
    radar_frame_free(&f, NULL);

    /* DWD frames cover matching/smaller inset views at compatible zoom, not distant/deep-zoom views. */
    radar_frame_t dwd;
    square_at(&dwd, 49.9, 15.3, 1, RADAR_LIGHT);
    map_view_t fix, brno, nboh, way_in;
    map_view_init(&fix, 499000, 153000, 5.5, 400, 279);
    TEST_ASSERT_TRUE(radar_frame_covers(&dwd, &fix));
    map_view_init(&brno, 491951, 166068, 6.5, 400, 279);
    TEST_ASSERT_TRUE(radar_frame_covers(&dwd, &brno));
    map_view_init(&nboh, 506000, 139000, 6.5, 130, 130);
    TEST_ASSERT_TRUE(radar_frame_covers(&dwd, &nboh));
    TEST_ASSERT_FALSE(radar_frame_covers(&dwd, &paris));
    map_view_init(&way_in, 499000, 153000, 8.0, 400, 279); /* > 2.2x finer scale */
    TEST_ASSERT_FALSE(radar_frame_covers(&dwd, &way_in));
    radar_frame_free(&dwd, NULL);
}

static int s_live;

static void *counted_alloc(size_t n)
{
    s_live++;
    return calloc(1, n);
}

static void counted_free(void *p)
{
    if (p != NULL) {
        s_live--;
    }
    free(p);
}

static void test_the_store_keeps_frames_in_order_and_frees_the_rest(void)
{
    const png_mem_t mem = { counted_alloc, counted_free };
    radar_store_t s;
    radar_store_init(&s, &mem);
    s_live = 0;
    const uint32_t t0 = 1790880000;
    static const uint32_t k_times[] = { 600, 300, 900, 600 }; /* out of order, and one twice */
    for (size_t i = 0; i < sizeof(k_times) / sizeof(k_times[0]); i++) {
        radar_frame_t f;
        TEST_ASSERT_TRUE(radar_frame_alloc(&f, 8, 8, &mem));
        f.time = t0 + k_times[i];
        radar_store_put(&s, &f, RADAR_LOOP_FRAMES);
    }
    TEST_ASSERT_EQUAL_INT(3, s.count);
    TEST_ASSERT_EQUAL_INT(3, s_live); /* the repeated time replaced its frame */
    TEST_ASSERT_EQUAL_UINT32(t0 + 300, s.frames[0].time);
    TEST_ASSERT_EQUAL_UINT32(t0 + 900, radar_store_newest(&s)->time);
    TEST_ASSERT_TRUE(radar_store_has(&s, t0 + 600));

    uint32_t missing[12];
    int n = radar_store_missing(&s, t0 + 900, 300, missing, 12);
    TEST_ASSERT_EQUAL_INT(9, n); /* the hour has 12 steps; 3 are kept */
    TEST_ASSERT_EQUAL_UINT32(t0, missing[0]); /* newest first */
    TEST_ASSERT_EQUAL_UINT32(t0 - 8 * 300, missing[8]);

    radar_store_keep(&s, 2); /* sync mode always ends: the hour goes */
    TEST_ASSERT_EQUAL_INT(2, s.count);
    TEST_ASSERT_EQUAL_INT(2, s_live);
    TEST_ASSERT_EQUAL_UINT32(t0 + 600, s.frames[0].time);
    radar_frame_t f;
    TEST_ASSERT_TRUE(radar_frame_alloc(&f, 8, 8, &mem));
    f.time = t0 + 1200;
    radar_store_put(&s, &f, 1); /* outside sync mode always: only the newest stays */
    TEST_ASSERT_EQUAL_INT(1, s.count);
    TEST_ASSERT_EQUAL_INT(1, s_live);
    TEST_ASSERT_EQUAL_UINT32(t0 + 1200, radar_store_newest(&s)->time);
    radar_store_clear(&s);
    TEST_ASSERT_EQUAL_INT(0, s_live);
    TEST_ASSERT_NULL(radar_store_newest(&s));
}

static void test_a_fetch_wants_the_hour_it_lacks(void)
{
    const uint32_t t0 = 1790880000;
    uint32_t out[RADAR_LOOP_FRAMES];
    TEST_ASSERT_EQUAL_INT(1, radar_wanted(t0, 300, 1, NULL, 0, out)); /* outside sync mode always: the newest */
    TEST_ASSERT_EQUAL_UINT32(t0, out[0]);
    const uint32_t have[] = { t0 - 300, t0 - 900, t0 - 6000 }; /* the last one is older than the hour */
    TEST_ASSERT_EQUAL_INT(10, radar_wanted(t0, 300, RADAR_LOOP_FRAMES, have, 3, out));
    TEST_ASSERT_EQUAL_UINT32(t0, out[0]); /* newest first */
    TEST_ASSERT_EQUAL_UINT32(t0 - 600, out[1]);
    TEST_ASSERT_EQUAL_UINT32(t0 - 3300, out[9]);
    TEST_ASSERT_EQUAL_INT(0, radar_wanted(t0 - 300, 300, 1, have, 3, out)); /* nothing new */
    TEST_ASSERT_EQUAL_INT(6, radar_wanted(t0, 600, 6, NULL, 0, out)); /* RainViewer's hour */
    TEST_ASSERT_EQUAL_UINT32(t0 - 3000, out[5]);
    TEST_ASSERT_EQUAL_INT(RADAR_LOOP_FRAMES, radar_wanted(t0, 300, 40, NULL, 0, out)); /* never more than kept */
}

static void test_a_frame_survives_its_file_and_a_damaged_file_is_refused(void)
{
    radar_frame_t f, back;
    TEST_ASSERT_TRUE(radar_frame_alloc(&f, 30, 10, NULL));
    f.time = 1790880000;
    f.source = RADAR_SOURCE_RAINVIEWER;
    f.mx0 = 1254222.15;
    f.my0 = 6702777.85;
    f.scale = 1555.7;
    radar_frame_set(&f, 29, 9, RADAR_HEAVY);
    radar_frame_set(&f, 3, 4, RADAR_LIGHT);
    static uint8_t file[512];
    size_t n = radar_frame_to_file(&f, file, sizeof(file));
    TEST_ASSERT_EQUAL_size_t(radar_frame_file_size(&f), n);
    TEST_ASSERT_EQUAL_size_t(RADAR_FILE_HEADER + 75, n); /* 300 pixels at 2 bits */
    TEST_ASSERT_TRUE(radar_frame_from_file(file, n, NULL, &back));
    TEST_ASSERT_EQUAL_UINT32(f.time, back.time);
    TEST_ASSERT_EQUAL_UINT8(RADAR_SOURCE_RAINVIEWER, back.source);
    TEST_ASSERT_EQUAL_UINT16(30, back.w);
    TEST_ASSERT_EQUAL_DOUBLE(f.my0, back.my0);
    TEST_ASSERT_EQUAL_DOUBLE(f.scale, back.scale);
    TEST_ASSERT_EQUAL_MEMORY(f.levels, back.levels, 75);
    radar_frame_free(&back, NULL);

    TEST_ASSERT_EQUAL_size_t(0, radar_frame_to_file(&f, file, n - 1)); /* too small: nothing written */
    n = radar_frame_to_file(&f, file, sizeof(file));
    TEST_ASSERT_FALSE(radar_frame_from_file(file, n - 1, NULL, &back)); /* cut short */
    file[RADAR_FILE_HEADER + 10] ^= 0x10;
    TEST_ASSERT_FALSE(radar_frame_from_file(file, n, NULL, &back)); /* its CRC */
    file[RADAR_FILE_HEADER + 10] ^= 0x10;
    file[0] ^= 1;
    TEST_ASSERT_FALSE(radar_frame_from_file(file, n, NULL, &back)); /* not a frame */
    TEST_ASSERT_NULL(back.levels);
    radar_frame_free(&f, NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dwd_covers_layers_steps_and_urls);
    RUN_TEST(test_dwd_frames_decode_rv_and_iconeu);
    RUN_TEST(test_rain_lands_where_it_falls_in_any_view);
    RUN_TEST(test_outside_the_data_the_edge_is_dotted);
    RUN_TEST(test_the_rainviewer_index_names_its_frames);
    RUN_TEST(test_rainviewer_tiles_cover_the_view);
    RUN_TEST(test_a_rainviewer_tile_decodes_by_its_colour_table);
    RUN_TEST(test_a_frame_covers_the_views_it_has_the_rain_for);
    RUN_TEST(test_the_store_keeps_frames_in_order_and_frees_the_rest);
    RUN_TEST(test_a_fetch_wants_the_hour_it_lacks);
    RUN_TEST(test_a_frame_survives_its_file_and_a_damaged_file_is_refused);
    return UNITY_END();
}
