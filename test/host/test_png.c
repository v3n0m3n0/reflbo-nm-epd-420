#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "png.h"
#include "unity.h"
#include "util_crc32.h"

/* The ČHMÚ frames and the RainViewer tile were fetched on 2026-10-01; their pixel counts come from
 * Pillow, so they check this reader against another one. */

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
    uint8_t *data = malloc(*len);
    TEST_ASSERT_EQUAL_size_t(*len, fread(data, 1, *len, f));
    fclose(f);
    return data;
}

typedef struct {
    int rows;
    long count[256]; /* palette images: pixels per index */
    long opaque, alpha_sum;
    int first_x, first_y;
    uint8_t first[4];
    uint8_t probe[4]; /* the pixel at (probe_x, probe_y) */
    int probe_x, probe_y;
} stats_t;

static void on_row(void *ctx, int y, const uint8_t *row, const png_info_t *info)
{
    stats_t *s = ctx;
    TEST_ASSERT_EQUAL_INT(s->rows, y);
    s->rows++;
    for (int x = 0; x < info->width; x++) {
        if (info->type == PNG_PALETTE) {
            s->count[row[x]]++;
            if (x == s->probe_x && y == s->probe_y) {
                s->probe[0] = row[x];
            }
            continue;
        }
        const uint8_t *px = row + 4 * x;
        if (px[3] != 0) {
            if (s->opaque == 0) {
                s->first_x = x;
                s->first_y = y;
                memcpy(s->first, px, 4);
            }
            s->opaque++;
            s->alpha_sum += px[3];
        }
        if (x == s->probe_x && y == s->probe_y) {
            memcpy(s->probe, px, 4);
        }
    }
}

static void test_a_dwd_rv_frame_decodes_as_rgba(void)
{
    size_t len;
    uint8_t *data = load("dwd_rv_rain.png", &len);
    png_info_t info;
    static stats_t s;
    memset(&s, 0, sizeof(s));
    s.probe_x = 225;
    s.probe_y = 0;
    TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(data, len, NULL, &info, on_row, &s));
    TEST_ASSERT_EQUAL_UINT16(400, info.width);
    TEST_ASSERT_EQUAL_UINT16(279, info.height);
    TEST_ASSERT_EQUAL_UINT8(PNG_RGBA, info.type);
    TEST_ASSERT_EQUAL_UINT8(8, info.depth);
    TEST_ASSERT_EQUAL_INT(279, s.rows);
    TEST_ASSERT_EQUAL_INT32(35734, s.opaque);
    TEST_ASSERT_EQUAL_INT32(5236042, s.alpha_sum);
    TEST_ASSERT_EQUAL_INT(225, s.first_x);
    TEST_ASSERT_EQUAL_INT(0, s.first_y);
    static const uint8_t k_magenta[4] = { 251, 0, 255, 255 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_magenta, s.probe, 4);
    free(data);
}

static void test_a_rainviewer_tile_decodes_as_rgba(void)
{
    size_t len;
    uint8_t *data = load("rainviewer_tile.png", &len);
    png_info_t info;
    static stats_t s;
    memset(&s, 0, sizeof(s));
    s.probe_x = 128;
    s.probe_y = 128;
    TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(data, len, NULL, &info, on_row, &s));
    TEST_ASSERT_EQUAL_UINT8(PNG_RGBA, info.type);
    TEST_ASSERT_EQUAL_UINT8(8, info.depth);
    TEST_ASSERT_EQUAL_UINT16(256, info.width);
    TEST_ASSERT_EQUAL_INT(256, s.rows);
    TEST_ASSERT_EQUAL_INT32(9297, s.opaque);
    TEST_ASSERT_EQUAL_INT32(1686742, s.alpha_sum);
    TEST_ASSERT_EQUAL_INT(64, s.first_x);
    TEST_ASSERT_EQUAL_INT(0, s.first_y);
    static const uint8_t k_first[4] = { 130, 123, 105, 73 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_first, s.first, 4);
    static const uint8_t k_clear[4] = { 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_clear, s.probe, 4);
    free(data);
}

/* ---- crafted files ---- */

static size_t put_chunk(uint8_t *out, const char *type, const uint8_t *body, uint32_t n)
{
    out[0] = (uint8_t)(n >> 24);
    out[1] = (uint8_t)(n >> 16);
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)n;
    memcpy(out + 4, type, 4);
    if (n > 0) {
        memcpy(out + 8, body, n);
    }
    uint32_t crc = util_crc32(util_crc32(0, type, 4), body, n);
    out[8 + n] = (uint8_t)(crc >> 24);
    out[9 + n] = (uint8_t)(crc >> 16);
    out[10 + n] = (uint8_t)(crc >> 8);
    out[11 + n] = (uint8_t)crc;
    return 12 + n;
}

/* A whole file around raw scanlines (each with its filter byte), compressed with zlib. */
static size_t make_png(uint8_t *out, uint32_t w, uint32_t h, uint8_t depth, uint8_t type, uint8_t interlace,
                       const uint8_t *raw, size_t raw_len)
{
    static const uint8_t k_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    memcpy(out, k_sig, 8);
    size_t n = 8;
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                         depth, type, 0, 0, interlace };
    n += put_chunk(out + n, "IHDR", ihdr, 13);
    if (type == PNG_PALETTE) {
        static const uint8_t k_plte[16 * 3] = {
            0, 0, 0,       255, 0, 0,     0, 255, 0,     0, 0, 255,
            255, 255, 0,   255, 0, 255,   0, 255, 255,   128, 128, 128,
            128, 0, 0,     0, 128, 0,     0, 0, 128,     128, 128, 0,
            128, 0, 128,   0, 128, 128,   192, 192, 192, 255, 255, 255,
        };
        uint32_t plte_bytes = depth == 4 ? 16 * 3 : 4 * 3;
        n += put_chunk(out + n, "PLTE", k_plte, plte_bytes);
    }
    static uint8_t z[4096];
    uLongf zn = sizeof(z);
    TEST_ASSERT_EQUAL_INT(Z_OK, compress(z, &zn, raw, (uLong)raw_len));
    n += put_chunk(out + n, "IDAT", z, (uint32_t)zn);
    n += put_chunk(out + n, "IEND", NULL, 0);
    return n;
}

typedef struct {
    uint8_t px[5][4]; /* palette indices, 4 x 5 */
    int rows;
} small_t;

static void on_small_row(void *ctx, int y, const uint8_t *row, const png_info_t *info)
{
    small_t *s = ctx;
    TEST_ASSERT_EQUAL_UINT16(4, info->width);
    memcpy(s->px[y], row, 4);
    s->rows++;
}

static void test_every_filter_type_is_undone(void)
{
    /* Rows filtered with None, Sub, Up, Average and Paeth; each decodes to the row in `want`. */
    static const uint8_t k_want[5][4] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 9, 9, 9 },
                                          { 2, 4, 6, 8 }, { 3, 1, 4, 1 } };
    uint8_t raw[5 * 5];
    for (int y = 0; y < 5; y++) {
        const uint8_t *cur = k_want[y];
        const uint8_t *up = y > 0 ? k_want[y - 1] : (const uint8_t[4]){ 0 };
        uint8_t *r = raw + y * 5;
        r[0] = (uint8_t)y; /* filter type = row number */
        for (int x = 0; x < 4; x++) {
            int a = x > 0 ? cur[x - 1] : 0, b = up[x], c = x > 0 ? up[x - 1] : 0;
            int pred = 0;
            switch (y) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            default: break;
            }
            r[1 + x] = (uint8_t)(cur[x] - pred);
        }
    }
    static uint8_t file[8192];
    size_t n = make_png(file, 4, 5, 8, PNG_PALETTE, 0, raw, sizeof(raw));
    png_info_t info;
    small_t s;
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(file, n, NULL, &info, on_small_row, &s));
    TEST_ASSERT_EQUAL_INT(5, s.rows);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_want, s.px, sizeof(k_want));
    TEST_ASSERT_EQUAL_UINT8(255, info.palette[2][3]); /* no tRNS: every entry opaque */
}

static void test_an_8bit_palette_with_trns_decodes(void)
{
    static const uint8_t k_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    static const uint8_t k_plte[4 * 3] = { 0, 0, 0, 10, 20, 30, 40, 50, 60, 255, 255, 255 };
    static const uint8_t k_trns[2] = { 0, 128 };
    static const uint8_t k_raw[2 * 5] = { 0, 0, 1, 2, 3, 0, 3, 2, 1, 0 };
    uint8_t img[512];
    memcpy(img, k_sig, 8);
    size_t n = 8;
    uint8_t ihdr[13] = { 0, 0, 0, 4, 0, 0, 0, 2, 8, PNG_PALETTE, 0, 0, 0 };
    n += put_chunk(img + n, "IHDR", ihdr, 13);
    n += put_chunk(img + n, "PLTE", k_plte, sizeof(k_plte));
    n += put_chunk(img + n, "tRNS", k_trns, sizeof(k_trns));
    uint8_t z[128];
    uLongf zn = sizeof(z);
    TEST_ASSERT_EQUAL_INT(Z_OK, compress(z, &zn, k_raw, sizeof(k_raw)));
    n += put_chunk(img + n, "IDAT", z, (uint32_t)zn);
    n += put_chunk(img + n, "IEND", NULL, 0);

    png_info_t info;
    static stats_t s;
    memset(&s, 0, sizeof(s));
    s.probe_x = 1;
    s.probe_y = 0;
    TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(img, n, NULL, &info, on_row, &s));
    TEST_ASSERT_EQUAL_UINT16(4, info.palette_size);
    TEST_ASSERT_EQUAL_UINT8(0, info.palette[0][3]);
    TEST_ASSERT_EQUAL_UINT8(128, info.palette[1][3]);
    TEST_ASSERT_EQUAL_UINT8(255, info.palette[2][3]);
    TEST_ASSERT_EQUAL_INT32(2, s.count[0]);
    TEST_ASSERT_EQUAL_INT32(2, s.count[1]);
    TEST_ASSERT_EQUAL_UINT8(1, s.probe[0]);
}

static void ignore_row(void *ctx, int y, const uint8_t *row, const png_info_t *info)
{
    (void)y;
    (void)row;
    (void)info;
    (*(int *)ctx)++;
}

static void test_broken_and_unsupported_files_are_refused(void)
{
    static uint8_t raw[5 * 5]; /* five None rows of zeros */
    static uint8_t file[8192];
    png_info_t info;
    int rows = 0;
    size_t n = make_png(file, 4, 5, 8, PNG_PALETTE, 0, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(file, n, NULL, &info, ignore_row, &rows));

    TEST_ASSERT_EQUAL_INT(PNG_ERR_FORMAT, png_decode(file, n - 13, NULL, &info, ignore_row, &rows)); /* cut short */
    file[20] ^= 1; /* a bit of IHDR's height: its CRC no longer matches */
    TEST_ASSERT_EQUAL_INT(PNG_ERR_FORMAT, png_decode(file, n, NULL, &info, ignore_row, &rows));
    file[20] ^= 1;
    file[1] = 'Q';
    TEST_ASSERT_EQUAL_INT(PNG_ERR_FORMAT, png_decode(file, n, NULL, &info, ignore_row, &rows)); /* not a PNG */
    file[1] = 'P';

    n = make_png(file, 4, 5, 8, PNG_PALETTE, 1, raw, sizeof(raw)); /* Adam7 */
    TEST_ASSERT_EQUAL_INT(PNG_ERR_UNSUPPORTED, png_decode(file, n, NULL, &info, ignore_row, &rows));
    n = make_png(file, 4, 5, 16, PNG_RGBA, 0, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(PNG_ERR_UNSUPPORTED, png_decode(file, n, NULL, &info, ignore_row, &rows));
    n = make_png(file, 4, 5, 8, 2, 0, raw, sizeof(raw)); /* RGB without alpha */
    TEST_ASSERT_EQUAL_INT(PNG_ERR_UNSUPPORTED, png_decode(file, n, NULL, &info, ignore_row, &rows));
    n = make_png(file, PNG_MAX_SIDE + 1, 5, 8, PNG_PALETTE, 0, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(PNG_ERR_TOO_BIG, png_decode(file, n, NULL, &info, ignore_row, &rows));
    TEST_ASSERT_EQUAL_INT(PNG_ERR_TOO_BIG, png_decode(file, PNG_MAX_BYTES + 1, NULL, &info, ignore_row, &rows));

    n = make_png(file, 4, 6, 8, PNG_PALETTE, 0, raw, sizeof(raw)); /* six rows declared, five sent */
    TEST_ASSERT_EQUAL_INT(PNG_ERR_INFLATE, png_decode(file, n, NULL, &info, ignore_row, &rows));
    raw[5] = 7; /* the second row's filter byte: no such filter */
    n = make_png(file, 4, 5, 8, PNG_PALETTE, 0, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(PNG_ERR_FORMAT, png_decode(file, n, NULL, &info, ignore_row, &rows));
    TEST_ASSERT_EQUAL_STRING("unsupported", png_err_name(PNG_ERR_UNSUPPORTED));
}

static int s_allocs, s_frees, s_fail_after;

static void *counting_alloc(size_t size)
{
    if (s_fail_after >= 0 && s_allocs >= s_fail_after) {
        return NULL;
    }
    s_allocs++;
    return malloc(size);
}

static void counting_free(void *p)
{
    if (p != NULL) {
        s_frees++;
    }
    free(p);
}

static void test_memory_comes_from_the_hooks_and_goes_back(void)
{
    static const char *const k_files[] = { "dwd_rv_rain.png", "dwd_iconeu_rain.png" };
    const png_mem_t mem = { counting_alloc, counting_free };
    png_info_t info;
    int rows = 0;
    for (size_t i = 0; i < sizeof(k_files) / sizeof(k_files[0]); i++) {
        size_t len;
        uint8_t *data = load(k_files[i], &len);
        s_allocs = s_frees = 0;
        s_fail_after = -1;
        TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(data, len, &mem, &info, ignore_row, &rows));
        TEST_ASSERT_EQUAL_INT(2, s_allocs);
        TEST_ASSERT_EQUAL_INT(s_allocs, s_frees);
        for (int fail = 0; fail < 2; fail++) {
            s_allocs = s_frees = 0;
            s_fail_after = fail;
            TEST_ASSERT_EQUAL_INT(PNG_ERR_NO_MEMORY, png_decode(data, len, &mem, &info, ignore_row, &rows));
            TEST_ASSERT_EQUAL_INT(s_allocs, s_frees);
        }
        free(data);
    }
}

typedef struct {
    uint8_t px[4][16];
    int rows;
    uint16_t width;
} sub_byte_t;

static void on_sub_byte_row(void *ctx, int y, const uint8_t *row, const png_info_t *info)
{
    sub_byte_t *s = ctx;
    TEST_ASSERT_EQUAL_UINT16(s->width, info->width);
    TEST_ASSERT_TRUE(y >= 0 && y < 4);
    memcpy(s->px[y], row, info->width);
    s->rows++;
}

static void check_sub_byte_case(uint16_t w, uint16_t h, uint8_t depth, const uint8_t *raw, size_t raw_len,
                                const uint8_t want[4][16])
{
    static uint8_t file[8192];
    size_t n = make_png(file, w, h, depth, PNG_PALETTE, 0, raw, raw_len);
    png_info_t info;
    sub_byte_t s;
    memset(&s, 0, sizeof(s));
    s.width = w;
    TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(file, n, NULL, &info, on_sub_byte_row, &s));
    TEST_ASSERT_EQUAL_UINT8(depth, info.depth);
    TEST_ASSERT_EQUAL_INT(h, s.rows);
    for (int y = 0; y < h; y++) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(want[y], s.px[y], w);
    }
}

static void test_sub_byte_palette_depths_unpack_to_indices(void)
{
    /* 4-bit w=5, h=3: None, Sub (bpp=1), Up (bpp=1), with non-zero trailing padding nibble 0x79 */
    static const uint8_t k_raw4[] = {
        0, 0xA3, 0xF1, 0x79,
        1, 0x24, 0x6A, 0x32,
        2, 0x32, 0x0D, 0x10,
    };
    static const uint8_t k_want4[4][16] = {
        { 0xA, 0x3, 0xF, 0x1, 0x7 },
        { 0x2, 0x4, 0x8, 0xE, 0xC },
        { 0x5, 0x6, 0x9, 0xB, 0xD },
    };
    check_sub_byte_case(5, 3, 4, k_raw4, sizeof(k_raw4), k_want4);

    /* 2-bit w=5, h=2: None, Sub (bpp=1), with non-zero padding bits 0x3F in row 0 */
    static const uint8_t k_raw2[] = {
        0, 0xD8, 0xBF,
        1, 0x2D, 0x93,
    };
    static const uint8_t k_want2[4][16] = {
        { 3, 1, 2, 0, 2 },
        { 0, 2, 3, 1, 3 },
    };
    check_sub_byte_case(5, 2, 2, k_raw2, sizeof(k_raw2), k_want2);

    /* 1-bit w=5, h=2: None and Up within a single packed byte, ignoring trailing padding bits */
    static const uint8_t k_raw1_5[] = {
        0, 0xB7,
        2, 0x91,
    };
    static const uint8_t k_want1_5[4][16] = {
        { 1, 0, 1, 1, 0 },
        { 0, 1, 0, 0, 1 },
    };
    check_sub_byte_case(5, 2, 1, k_raw1_5, sizeof(k_raw1_5), k_want1_5);

    /* 1-bit w=11, h=2: None and Sub across byte boundary */
    static const uint8_t k_raw1_11[] = {
        0, 0xB2, 0xDF,
        1, 0x69, 0xF7,
    };
    static const uint8_t k_want1_11[4][16] = {
        { 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 },
        { 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1 },
    };
    check_sub_byte_case(11, 2, 1, k_raw1_11, sizeof(k_raw1_11), k_want1_11);

    /* Unsupported sub-byte RGBA and 16-bit / invalid palette bit depths */
    static uint8_t file[8192];
    static const uint8_t dummy[16] = { 0 };
    png_info_t info;
    int rows = 0;
    static const struct {
        uint8_t depth;
        uint8_t type;
    } k_bad[] = {
        { 4, PNG_RGBA },
        { 2, PNG_RGBA },
        { 1, PNG_RGBA },
        { 16, PNG_PALETTE },
        { 3, PNG_PALETTE },
    };
    for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
        size_t n = make_png(file, 4, 1, k_bad[i].depth, k_bad[i].type, 0, dummy, sizeof(dummy));
        TEST_ASSERT_EQUAL_INT(PNG_ERR_UNSUPPORTED, png_decode(file, n, NULL, &info, ignore_row, &rows));
    }
}

static void test_a_dwd_iconeu_frame_decodes_4bit_palette(void)
{
    size_t len;
    uint8_t *data = load("dwd_iconeu_rain.png", &len);
    png_info_t info;
    static stats_t s;
    memset(&s, 0, sizeof(s));
    s.probe_x = 72;
    s.probe_y = 251;
    TEST_ASSERT_EQUAL_INT(PNG_OK, png_decode(data, len, NULL, &info, on_row, &s));
    TEST_ASSERT_EQUAL_UINT16(400, info.width);
    TEST_ASSERT_EQUAL_UINT16(279, info.height);
    TEST_ASSERT_EQUAL_UINT8(PNG_PALETTE, info.type);
    TEST_ASSERT_EQUAL_UINT8(4, info.depth);
    TEST_ASSERT_EQUAL_UINT16(16, info.palette_size);
    TEST_ASSERT_EQUAL_UINT8(0, info.palette[0][3]); /* tRNS: transparent background */
    static const uint8_t k_light_rain[4] = { 192, 192, 192, 255 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_light_rain, info.palette[1], 4);
    static const uint8_t k_mod_rain[4] = { 254, 254, 192, 255 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_mod_rain, info.palette[2], 4);
    TEST_ASSERT_EQUAL_UINT8(0, info.palette[15][3]);
    TEST_ASSERT_EQUAL_INT(279, s.rows);
    TEST_ASSERT_EQUAL_INT32(104678, s.count[0]);
    TEST_ASSERT_EQUAL_INT32(6871, s.count[1]);
    TEST_ASSERT_EQUAL_INT32(51, s.count[2]);
    for (int i = 3; i < 256; i++) {
        TEST_ASSERT_EQUAL_INT32(0, s.count[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(2, s.probe[0]);
    free(data);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_dwd_rv_frame_decodes_as_rgba);
    RUN_TEST(test_a_rainviewer_tile_decodes_as_rgba);
    RUN_TEST(test_every_filter_type_is_undone);
    RUN_TEST(test_an_8bit_palette_with_trns_decodes);
    RUN_TEST(test_broken_and_unsupported_files_are_refused);
    RUN_TEST(test_memory_comes_from_the_hooks_and_goes_back);
    RUN_TEST(test_sub_byte_palette_depths_unpack_to_indices);
    RUN_TEST(test_a_dwd_iconeu_frame_decodes_4bit_palette);
    return UNITY_END();
}
