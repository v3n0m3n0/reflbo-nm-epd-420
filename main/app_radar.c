#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "map_data.h"
#include "st7305.h"
#include "storage.h"
#include "timekeeping.h"

/* The weather radar on the device (spec §11.2): the frames kept in PSRAM, the newest also in
 * /fs/state/radar.bin, and what the views draw. It belongs to the app task; the sync task fetches. */

static const char *TAG = "app_radar";

#define FRAME_PATH "/fs/state/radar.bin"
#define FRAME_FILE_MAX (RADAR_FILE_HEADER + 3 * 256 * 3 * 256 / 4) /* RainViewer's 3 x 3 tiles */
#define SAVE_EVERY_S 1800 /* sync mode `always`: the file follows its new frames at most this often */
#define LOOP_STEP_MS 333 /* the loop: about 3 frames a second (spec §11.2) */

extern const uint8_t k_map_start[] asm("_binary_map_bin_start");
extern const uint8_t k_map_end[] asm("_binary_map_bin_end");

static radar_store_t s_store;
static bool s_ready;
static bool s_loaded;       /* the file was read since the boot or the wake */
static uint32_t s_saved_at; /* the time of the frame in the file; 0 = none known */
static map_data_t s_map;
static int s_map_state; /* 0 not opened yet, 1 open, -1 broken */
static app_radar_status_t s_status;
static uint32_t s_gen;     /* bumped on app_radar_invalidate() to reject in-flight stale fetches */
static uint32_t s_req_gen; /* generation captured when app_radar_request() builds the request */
static ui_radar_t s_ui;
static uint8_t s_loop_at, s_loop_count; /* the loop (D28): the frame shown of how many; 0 when it doesn't run */
static int64_t s_loop_next_ms;          /* app_uptime_ms() of its next frame */
static radar_frame_t s_demo_frame;
static bool s_demo;
RTC_DATA_ATTR static time_t s_auto_try_rtc;
static int64_t s_auto_try_ms = -1;

static void ready(void)
{
    if (!s_ready) {
        radar_store_init(&s_store, &radar_fetch_mem);
        s_ready = true;
    }
}

static uint8_t source_now(void)
{
    const settings_t *set = app_settings();
    double lat = set->wx_lat_e4 / 1e4;
    double lon = set->wx_lon_e4 / 1e4;
    if (set->wx_source == SETTINGS_WX_RADAR_RAINVIEWER) {
        return RADAR_SOURCE_RAINVIEWER;
    }
    if (set->wx_source == SETTINGS_WX_RADAR_DWD) {
        return radar_dwd_covers(lat, lon) ? RADAR_SOURCE_DWD : RADAR_SOURCE_RAINVIEWER;
    }
    map_view_t v;
    map_view_init(&v, set->wx_lat_e4, set->wx_lon_e4, set->wx_zoom_q / 4.0f, RADAR_VIEW_W, RADAR_VIEW_H);
    return (radar_dwd_covers(lat, lon) && radar_dwd_layer_for_view(&v) == RADAR_DWD_LAYER_RV)
               ? RADAR_SOURCE_DWD
               : RADAR_SOURCE_RAINVIEWER;
}

/* One frame, or in sync mode `always` the loop's hour (D28): 12 of DWD RV, 6 of ICON-EU/RainViewer. */
static int keep_count(void)
{
    const settings_t *set = app_settings();
    if (set->sync_mode != SETTINGS_SYNC_ALWAYS) {
        return 1;
    }
    if (source_now() != RADAR_SOURCE_DWD) {
        return 3600 / RADAR_RV_STEP_S;
    }
    map_view_t v;
    map_view_init(&v, set->wx_lat_e4, set->wx_lon_e4, set->wx_zoom_q / 4.0, RADAR_VIEW_W,
                  RADAR_VIEW_H);
    return radar_dwd_layer_for_view(&v) == RADAR_DWD_LAYER_RV ? RADAR_LOOP_FRAMES : 6;
}

uint32_t app_radar_step_s(void)
{
    const settings_t *set = app_settings();
    if (source_now() == RADAR_SOURCE_DWD) {
        return radar_dwd_step_s(set->wx_lat_e4 / 1e4, set->wx_lon_e4 / 1e4,
                                set->wx_zoom_q / 4.0);
    }
    return RADAR_RV_STEP_S;
}

/* A frame with the rain for the view as it is now: accepts DWD or RainViewer fallback whenever
 * the stored grid covers the current view. */
static bool fits(const radar_frame_t *f)
{
    const settings_t *set = app_settings();
    bool dwd_ok = radar_dwd_covers(set->wx_lat_e4 / 1e4, set->wx_lon_e4 / 1e4);
    if (f->source != RADAR_SOURCE_RAINVIEWER && !(f->source == RADAR_SOURCE_DWD && dwd_ok)) {
        return false;
    }
    map_view_t v;
    map_view_init(&v, set->wx_lat_e4, set->wx_lon_e4, set->wx_zoom_q / 4.0, RADAR_VIEW_W, RADAR_VIEW_H);
    return radar_frame_covers(f, &v);
}

/* A new centre or zoom, a new mode: frames that no longer fit go, and so do those beyond the keep. */
static void tidy(void)
{
    ready();
    const radar_frame_t *newest = radar_store_newest(&s_store);
    if (newest != NULL && !fits(newest)) {
        ESP_LOGI(TAG, "the view moved: its frames are dropped");
        radar_store_clear(&s_store);
    }
    radar_store_keep(&s_store, keep_count()); /* sync mode `always` ended: the newest stays */
}

/* The newest frame from its file, once a boot or a wake: a deep sleep loses PSRAM. */
static void load(void)
{
    ready();
    if (s_loaded || s_store.count > 0) {
        return;
    }
    s_loaded = true;
    if (storage_init() != ESP_OK) {
        return;
    }
    FILE *file = fopen(FRAME_PATH, "rb");
    if (file == NULL) {
        return;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *buf = size > 0 && size <= FRAME_FILE_MAX ? heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM) : NULL;
    bool read = buf != NULL && fread(buf, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    radar_frame_t f;
    if (read && radar_frame_from_file(buf, (size_t)size, &radar_fetch_mem, &f)) {
        s_saved_at = f.time;
        ESP_LOGI(TAG, "frame of %lu from %s", (unsigned long)f.time, FRAME_PATH);
        radar_store_put(&s_store, &f, keep_count());
        tidy(); /* a frame for another view goes */
    } else if (buf != NULL) {
        ESP_LOGW(TAG, "%s: not a frame of this firmware; left out", FRAME_PATH);
    }
    heap_caps_free(buf);
}

/* The newest frame into its file, so a power-off or a deep sleep keeps it; in sync mode `always`,
 * where one comes every 5 or 10 minutes, at most every SAVE_EVERY_S, to spare the flash. */
static void save_newest(void)
{
    const radar_frame_t *f = radar_store_newest(&s_store);
    if (f == NULL || f->time == s_saved_at) {
        return;
    }
    if (app_settings()->sync_mode == SETTINGS_SYNC_ALWAYS && s_saved_at != 0 && f->time < s_saved_at + SAVE_EVERY_S) {
        return;
    }
    if (storage_init() != ESP_OK) {
        return;
    }
    size_t n = radar_frame_file_size(f);
    uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return;
    }
    radar_frame_to_file(f, buf, n);
    esp_err_t err = storage_write_atomic(FRAME_PATH, (const char *)buf, n);
    heap_caps_free(buf);
    if (err == ESP_OK) {
        s_saved_at = f->time;
    } else {
        ESP_LOGW(TAG, "%s not saved: %s", FRAME_PATH, esp_err_to_name(err));
    }
}

void app_radar_request(radar_fetch_req_t *out)
{
    load(); /* the frame in the file is had already */
    tidy();
    const settings_t *set = app_settings();
    memset(out, 0, sizeof(*out));
    out->lat_e4 = set->wx_lat_e4;
    out->lon_e4 = set->wx_lon_e4;
    out->zoom_q = set->wx_zoom_q;
    out->source = source_now();
    out->want = (uint8_t)keep_count();
    for (int i = 0; i < s_store.count; i++) {
        out->have[out->have_count++] = s_store.frames[i].time;
    }
    out->now = timekeeping_valid() ? (uint32_t)time(NULL) : 0; /* a sync's own NTP time replaces it */
    s_req_gen = s_gen;
}

void app_radar_apply(radar_fetch_result_t *res, uint8_t result, const char *detail)
{
    if (s_req_gen != s_gen) {
        for (int i = 0; i < res->count; i++) {
            radar_frame_free(&res->frames[i], &radar_fetch_mem);
        }
        res->count = 0;
        return;
    }
    app_radar_loop_stop(); /* new frames move the old ones along */
    tidy();
    int added = 0;
    for (int i = 0; i < res->count; i++) {
        radar_frame_t *f = &res->frames[i];
        if (fits(f)) {
            radar_store_put(&s_store, f, keep_count()); /* the store takes it */
            added++;
        } else {
            radar_frame_free(f, &radar_fetch_mem); /* the view moved while it came */
        }
    }
    res->count = 0;
    if (result != SYNC_STEP_NOT_RUN) { /* a sync that never got to it leaves the last fetch's status */
        s_status.fetched_at = time(NULL);
        s_status.ok = result == SYNC_STEP_OK;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", s_status.ok ? "" : detail);
    }
    if (added > 0) {
        save_newest();
    }
}

void app_radar_invalidate(void)
{
    ready();
    s_gen++;
    s_loop_count = 0;
    radar_store_clear(&s_store);
    s_saved_at = 0;
    s_loaded = false;
    remove(FRAME_PATH);
    memset(&s_status, 0, sizeof(s_status));
    s_auto_try_ms = -1;
    s_auto_try_rtc = 0;
}

void app_radar_status(app_radar_status_t *out)
{
    tidy();
    *out = s_status;
    out->frames = s_demo ? 1 : (uint8_t)s_store.count;
    const radar_frame_t *f = s_demo ? &s_demo_frame : radar_store_newest(&s_store);
    out->frame_at = f != NULL ? f->time : 0;
    out->source = f != NULL ? f->source : source_now();
}

void app_radar_demo(bool on)
{
    if (on == s_demo) {
        return;
    }
    if (on) {
        ready();
        const settings_t *set = app_settings();
        map_view_t v;
        map_view_init(&v, set->wx_lat_e4, set->wx_lon_e4, set->wx_zoom_q / 4.0, RADAR_VIEW_W, RADAR_VIEW_H);
        radar_rv_tiles_t t;
        radar_rv_tiles(&v, &t);
        if (s_demo_frame.levels != NULL) {
            radar_frame_free(&s_demo_frame, &radar_fetch_mem);
        }
        if (radar_rv_frame_alloc(&s_demo_frame, &t, (uint32_t)time(NULL), &radar_fetch_mem)) {
            int cx = s_demo_frame.w / 2;
            int cy = s_demo_frame.h / 2;
            int r_outer = (s_demo_frame.w < s_demo_frame.h ? s_demo_frame.w : s_demo_frame.h) / 3;
            int r_mid = r_outer * 2 / 3;
            int r_core = r_outer / 3;
            if (r_outer < 30) r_outer = 30;
            if (r_mid < 20) r_mid = 20;
            if (r_core < 10) r_core = 10;
            for (int y = 0; y < s_demo_frame.h; y++) {
                for (int x = 0; x < s_demo_frame.w; x++) {
                    int dx = x - cx;
                    int dy = y - cy;
                    int dist_sq = dx * dx + dy * dy;
                    if (dist_sq <= r_core * r_core) {
                        radar_frame_set(&s_demo_frame, x, y, RADAR_HEAVY);
                    } else if (dist_sq <= r_mid * r_mid) {
                        radar_frame_set(&s_demo_frame, x, y, RADAR_MODERATE);
                    } else if (dist_sq <= r_outer * r_outer) {
                        radar_frame_set(&s_demo_frame, x, y, RADAR_LIGHT);
                    }
                }
            }
            s_demo = true;
            ESP_LOGI(TAG, "radar demo ON: synthetic rain cell generated (%u x %u)", s_demo_frame.w, s_demo_frame.h);
        } else {
            ESP_LOGE(TAG, "failed to allocate radar demo frame");
        }
    } else {
        if (s_demo_frame.levels != NULL) {
            radar_frame_free(&s_demo_frame, &radar_fetch_mem);
        }
        s_demo = false;
        ESP_LOGI(TAG, "radar demo OFF");
    }
}

bool app_radar_demo_active(void)
{
    return s_demo;
}

/* The preset draws the weather radar: the Radar layout, or a slot's or a cell's map. */
static bool shows_weather(const ui_preset_t *p)
{
    if (p->layout == UI_LAYOUT_RADAR) {
        return true;
    }
    for (int i = 0; i < ui_preset_slots(p); i++) {
        if (p->slots[i] == UI_FIELD_RAIN_MAP) {
            return true;
        }
    }
    return false;
}

const map_data_t *app_radar_map(void)
{
    if (s_map_state == 0) {
        /* the image's own checksum covers it: only its header is checked (map_data_verify is the host's) */
        s_map_state = map_data_open(&s_map, k_map_start, (size_t)(k_map_end - k_map_start)) ? 1 : -1;
        if (s_map_state < 0) {
            ESP_LOGE(TAG, "the built-in map doesn't open");
        }
    }
    return s_map_state > 0 ? &s_map : NULL;
}

void app_radar_prepare(const ui_preset_t *p)
{
    bool weather = shows_weather(p);
    if (weather || p->layout == UI_LAYOUT_FLIGHTS) {
        (void)app_radar_map();
    }
    if (weather) {
        load();
    }
}

void app_radar_maybe_refresh(const ui_preset_t *p)
{
    const settings_t *set = app_settings();
    if (!shows_weather(p) || s_demo || !(set->sync_steps & SETTINGS_STEP_RADAR) ||
        app_state()->critical || app_ui_night() || app_config_active() ||
        app_focus_mode_active()) {
        return;
    }
    load();
    const radar_frame_t *newest = radar_store_newest(&s_store);
    time_t now = time(NULL);
    int64_t up_ms = app_uptime_ms();
    bool stale = (newest == NULL) ||
                 (timekeeping_valid() && now > (time_t)newest->time &&
                  (now - (time_t)newest->time) >= 1200);
    bool cooled_down = (s_auto_try_ms < 0 || (up_ms - s_auto_try_ms) >= 300000) &&
                       (!timekeeping_valid() || s_auto_try_rtc == 0 || now < s_auto_try_rtc ||
                        (now - s_auto_try_rtc) >= 300) &&
                       (s_status.fetched_at == 0 || !timekeeping_valid() ||
                        now < s_status.fetched_at || (now - s_status.fetched_at) >= 300);
    if (stale && cooled_down) {
        s_auto_try_ms = up_ms;
        if (timekeeping_valid()) {
            s_auto_try_rtc = now;
        }
        app_sync_radar_due_now();
    }
}

const ui_radar_t *app_radar_ui(void)
{
    tidy();
    const settings_t *set = app_settings();
    bool loop = s_loop_count > 0 && s_loop_at < s_store.count;
    s_ui = (ui_radar_t){ .map = s_map_state > 0 ? &s_map : NULL, .home_lat_e4 = set->lat_e4,
                         .home_lon_e4 = set->lon_e4,
                         .frame = s_demo ? &s_demo_frame : (loop ? &s_store.frames[s_loop_at] : radar_store_newest(&s_store)),
                         .loop_at = loop ? s_loop_at : 0, .loop_count = loop ? s_loop_count : 0,
                         .wx_lat_e4 = set->wx_lat_e4, .wx_lon_e4 = set->wx_lon_e4, .wx_zoom_q = set->wx_zoom_q,
                         .fl_lat_e4 = set->fl_lat_e4, .fl_lon_e4 = set->fl_lon_e4,
                         .fl_range_km = set->fl_range_km,
                         .fl_always = set->sync_mode == SETTINGS_SYNC_ALWAYS };
    app_flights_fill(&s_ui);
    return &s_ui;
}

bool app_radar_loop_start(void)
{
    tidy();
    if (app_settings()->sync_mode != SETTINGS_SYNC_ALWAYS || s_store.count < 2) {
        return false; /* outside sync mode `always` there is one frame (spec §11.2) */
    }
    s_loop_count = (uint8_t)s_store.count;
    s_loop_at = 0;
    s_loop_next_ms = app_uptime_ms() + LOOP_STEP_MS;
    ESP_LOGI(TAG, "loop: %u frames", s_loop_count);
    app_ui_render();
    return true;
}

void app_radar_loop_tick(void)
{
    if (s_loop_count == 0 || app_uptime_ms() < s_loop_next_ms) {
        return;
    }
    if (++s_loop_at >= s_loop_count || s_loop_at >= s_store.count) {
        app_radar_loop_stop(); /* it ends on the newest */
        return;
    }
    s_loop_next_ms += LOOP_STEP_MS;
    app_ui_render();
}

void app_radar_loop_stop(void)
{
    if (s_loop_count == 0) {
        return;
    }
    s_loop_count = 0;
    app_ui_render();
}

int64_t app_radar_loop_deadline_ms(void)
{
    return s_loop_count > 0 ? s_loop_next_ms : 0;
}
