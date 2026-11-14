#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "display.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "lang.h"
#include "lora_widget.h"
#include "netmgr.h"
#include "power.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "st7305.h"
#include "storage.h"
#include "timekeeping.h"
#include "ui_dashboard.h"
#include "ui_lora.h"
#include "ui_screens.h"
#include "util_base64.h"
#include "util_snapshot.h"
#include "util_time.h"
#include "webui.h"

#define TOAST_MS 2500
#define DISPLAY_SUPPORTS_SECONDS 0 /* SSD1683 e-paper: no 1 Hz refresh */

static const char *TAG = "app_ui";

static app_ui_state_t s;
static ui_presets_t s_presets; /* through deep sleep in their own block (app_ui_export_presets) */
/* The config files' text: scratch buffers in PSRAM (AGENTS.md §8). */
EXT_RAM_BSS_ATTR static char s_file[UI_PRESETS_JSON_MAX];
EXT_RAM_BSS_ATTR static char s_settings_base[SETTINGS_FILE_MAX]; /* settings.json as read: unknown keys stay */
static char s_err[UI_PRESET_ERR_LEN];
static char s_toast[64];
static int64_t s_toast_until_ms;
static ui_lora_t s_lora_ui;
static uint32_t s_last_lora_pkt_count;
static uint32_t s_last_lora_sent_count;
static uint32_t s_last_lora_msg_count;
static uint32_t s_last_lora_msg_seq;
static uint32_t s_last_lora_adv_seq;
static bool s_last_lora_ble;
static bool s_focus_synced;
static bool s_focus_sync_queued;

#define LEARN_PATH "/fs/state/battery_learn.txt" /* the battery learning session in base64 (D21) */
#define FORECAST_PATH "/fs/state/datastore.bin"    /* the last weather and air quality (spec §6) */
#define FORECAST_MAGIC 0x72666366u /* "rfcf" */
#define FORECAST_VERSION 2 /* 2: the rain in quarter hours (M6) */
#define LEARN_TEXT_MAX ((BATTERY_LEARN_PACKED_MAX + 2) / 3 * 4 + 1)
_Static_assert(SETTINGS_BAT_CURVE_POINTS == BATTERY_CURVE_POINTS, "settings keep a learned curve whole");

static void default_settings(settings_t *out)
{
    *out = (settings_t){
        .language = "en",
        .clock_24h = true,
        .sensors_every_min = CONFIG_REFLBO_SENSOR_INTERVAL_MIN,
        .temp_offset_c100 = CONFIG_REFLBO_TEMP_OFFSET_C10 * 10,
        .hum_offset_pct100 = CONFIG_REFLBO_HUM_OFFSET_PCT10 * 10,
        .display_every_min = CONFIG_REFLBO_DISPLAY_UPDATE_MIN,
        .lpm_quarter_hz = 4, /* 1 Hz (D12) */
        .lat_e4 = CONFIG_REFLBO_LOCATION_LAT_E4,
        .lon_e4 = CONFIG_REFLBO_LOCATION_LON_E4,
        .bat_cal = SETTINGS_BAT_CURVE,
        .bat_empty_mv = BATTERY_EMPTY_MV,
        .bat_full_mv = BATTERY_FULL_MV,
    };
    settings_sync_defaults(out);
    settings_radar_defaults(out);
    settings_solar_defaults(out);
    settings_pc_defaults(out); /* M7 */
    settings_lora_defaults(out);
    snprintf(out->place, sizeof(out->place), "%s", CONFIG_REFLBO_LOCATION_NAME);
    snprintf(out->tz_posix, sizeof(out->tz_posix), "%s", CONFIG_REFLBO_TZ);
    snprintf(out->tz_iana, sizeof(out->tz_iana), "%s", CONFIG_REFLBO_TZ_NAME);
}

static esp_err_t save_battery_cal_cb(const battery_cal_t *cal)
{
    s.settings.bat_cal = cal->method == BATTERY_CAL_MANUAL    ? SETTINGS_BAT_MANUAL
                         : cal->method == BATTERY_CAL_LEARNED ? SETTINGS_BAT_LEARNED
                                                              : SETTINGS_BAT_CURVE;
    s.settings.bat_empty_mv = cal->empty_mv;
    s.settings.bat_full_mv = cal->full_mv;
    esp_err_t err = app_ui_save_settings();
    app_ui_apply_settings();
    return err;
}

void app_ui_defaults(void)
{
    memset(&s, 0, sizeof(s));
    ds_init(&s.ds);
    default_settings(&s.settings);
    ui_presets_defaults(&s_presets);
    s_focus_synced = app_focus_mode_active();
    s_focus_sync_queued = false;
    sensors_set_battery_cal_save_cb(save_battery_cal_cb);
}

static bool parse_settings(const char *text, void *ctx)
{
    settings_t defaults;
    default_settings(&defaults);
    bool ok = settings_from_json(text, &defaults, ctx, s_err, sizeof(s_err));
    if (!ok) {
        ESP_LOGW(TAG, "settings.json: %s", s_err);
    }
    return ok;
}

static bool parse_presets(const char *text, void *ctx)
{
    bool ok = ui_presets_from_json(text, ctx, s_err, sizeof(s_err));
    if (!ok) {
        ESP_LOGW(TAG, "presets.json: %s", s_err);
    }
    return ok;
}

/* ESP_OK, ESP_ERR_NOT_FOUND (no file yet), or ESP_ERR_INVALID_RESPONSE: it existed, but nothing
 * in it parsed, so the defaults are in use. */
static esp_err_t load_one(const char *path, char *buf, size_t buf_size, storage_parse_t parse, void *target,
                          size_t size, const void *defaults)
{
    bool from_backup = false;
    esp_err_t err = storage_load(path, buf, buf_size, parse, target, &from_backup);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s loaded%s", path, from_backup ? " from the backup" : "");
        return ESP_OK;
    }
    memcpy(target, defaults, size); /* a failed parse may have left it half-written */
    ESP_LOGI(TAG, "%s: %s; using defaults", path, err == ESP_ERR_NOT_FOUND ? "not there yet" : "invalid");
    return err == ESP_ERR_NOT_FOUND ? err : ESP_ERR_INVALID_RESPONSE;
}

void app_ui_load(void)
{
    sensors_set_battery_cal_save_cb(save_battery_cal_cb);
    settings_t settings_defaults;
    default_settings(&settings_defaults);
    EXT_RAM_BSS_ATTR static ui_presets_t presets_defaults; /* 2 KB with 24 cells (M6c): not on the app task's stack */
    ui_presets_defaults(&presets_defaults);
    s.settings = settings_defaults;
    s_presets = presets_defaults;
    s.cycle_at = 0; /* armed by the first tick, once the RTC has set the clock */
    s_settings_base[0] = '\0';
    s_focus_synced = app_focus_mode_active();
    s_focus_sync_queued = false;
    esp_err_t err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage: %s; settings and presets use their defaults", esp_err_to_name(err));
        return;
    }
    esp_err_t settings = load_one(STORAGE_SETTINGS_PATH, s_settings_base, sizeof(s_settings_base), parse_settings,
                                  &s.settings, sizeof(s.settings), &settings_defaults);
    if (settings != ESP_OK) {
        s_settings_base[0] = '\0'; /* nothing worth keeping in it */
    }
    s.first_run = settings == ESP_ERR_NOT_FOUND; /* a new board, or a factory reset (spec §5.5) */
    esp_err_t presets = load_one(STORAGE_PRESETS_PATH, s_file, sizeof(s_file), parse_presets, &s_presets,
                                 sizeof(s_presets), &presets_defaults);
    if (presets == ESP_OK && ui_presets_offer_builtins(&s_presets)) { /* spec §5.4: the radars, once */
        ESP_LOGI(TAG, "presets.json: offered the built-in presets it didn't have");
        app_ui_save_presets();
    }
    s_focus_synced = app_focus_mode_active();
    if (settings == ESP_ERR_INVALID_RESPONSE || presets == ESP_ERR_INVALID_RESPONSE) { /* spec §14.3: say so */
        app_ui_toast(lang_str(lang_get(s.settings.language), LS_T_DEFAULTS));
    }
    app_lora_identity_init(!s.first_run);
    if (settings == ESP_OK && s.settings.lora.channel_psk[0] != '\0' &&
        strcmp(s.settings.lora.channel_psk, LORA_DEFAULT_PUBLIC_PSK) != 0 &&
        !app_secret_set(SETTINGS_SECRET_LORA_PSK)) {
        if (app_lora_sync_psk_secret(s.settings.lora.channel_psk) == ESP_OK) {
            app_ui_save_settings(); /* strips legacy plaintext channel_psk from /fs/cfg/settings.json */
        }
    }
}

void app_ui_export(app_ui_state_t *out)
{
    *out = s;
}

void app_ui_import(const app_ui_state_t *in)
{
    s = *in;
}

void app_ui_export_presets(ui_presets_t *out)
{
    *out = s_presets;
}

void app_ui_import_presets(const ui_presets_t *in)
{
    s_presets = *in;
    s_focus_synced = app_focus_mode_active();
    s_focus_sync_queued = false;
}

app_ui_state_t *app_state(void)
{
    return &s;
}

const settings_t *app_settings(void)
{
    return &s.settings;
}

ds_t *app_ds(void)
{
    return &s.ds;
}

ui_presets_t *app_presets(void)
{
    return &s_presets;
}

void app_ui_apply_settings(void)
{
    timekeeping_init(s.settings.tz_posix);
    sensors_set_offsets(s.settings.temp_offset_c100, s.settings.hum_offset_pct100);
    battery_cal_t cal = { .method = s.settings.bat_cal == SETTINGS_BAT_MANUAL    ? BATTERY_CAL_MANUAL
                                    : s.settings.bat_cal == SETTINGS_BAT_LEARNED ? BATTERY_CAL_LEARNED
                                                                                 : BATTERY_CAL_CURVE,
                          .empty_mv = s.settings.bat_empty_mv, .full_mv = s.settings.bat_full_mv };
    memcpy(cal.learned_mv, s.settings.bat_learned_mv, sizeof(cal.learned_mv));
    sensors_set_battery_cal(&cal);
    /* Spec §5.1: stale after 15 min, but never before the next reading is due. */
    uint32_t ttl = (uint32_t)s.settings.sensors_every_min * 120u;
    ttl = ttl < 900 ? 900 : ttl;
    for (int f = 0; f < DS_FIELD_COUNT; f++) {
        ds_set_ttl(&s.ds, (ds_field_t)f, ttl);
    }
    lora_widget_set_location(s.settings.lat_e4, s.settings.lon_e4);
    sensors_battery_t bat = sensors_battery(time(NULL));
    if (bat.smoothed_mv > 0) {
        lora_widget_set_battery((uint16_t)bat.smoothed_mv, (uint8_t)(bat.level >= 0 ? bat.level : 0));
    }
    app_lora_apply();
    if (!s.settings.lora.enabled && s_presets.presets[s_presets.active].layout == UI_LAYOUT_LORA) {
        app_ui_select(ui_presets_next_with_lora(&s_presets, s.settings.sync_mode == SETTINGS_SYNC_ALWAYS, false), false);
    }
    if (s.cold_boot_at == 0 && timekeeping_valid()) {
        s.cold_boot_at = time(NULL); /* Info > Uptime counts from the first valid clock after a cold boot */
    }
}

static int32_t local_day(const struct tm *local)
{
    return (int32_t)util_days_from_civil(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday);
}

ui_wifi_mark_t app_ui_wifi_state(void)
{
    if (app_focus_mode_active() || app_config_shows_setup() || !app_net_ready()) {
        return UI_WIFI_NONE;
    }
    netmgr_status_t ns;
    netmgr_status(&ns);
    if (ns.state == NETMGR_STATION && ns.ip[0] != '\0') {
        return UI_WIFI_ON;
    }
    if (ns.state == NETMGR_JOINING || (app_sync_holds_wifi() && !app_config_active())) {
        return UI_WIFI_REJOINING;
    }
    return UI_WIFI_NONE;
}

void app_ui_context(ui_context_t *ctx)
{
    time_t now = time(NULL);
    *ctx = (ui_context_t){ .now = now, .time_valid = timekeeping_valid(), .ds = &s.ds,
                           .lang = lang_get(s.settings.language), .clock_24h = s.settings.clock_24h,
                           .fahrenheit = s.settings.fahrenheit,
                           .web_session = (app_config_active() || app_sync_lan_ui()) && webui_session_active(),
                           .lat_e4 = s.settings.lat_e4, .lon_e4 = s.settings.lon_e4,
                           .sync = app_sync_running()  ? UI_SYNC_RUNNING
                                   : app_sync_failed() ? UI_SYNC_FAILED
                                                       : UI_SYNC_IDLE,
                           .wifi = app_ui_wifi_state(),
                           .sync_ok_at = app_sync_ok_at() };
    localtime_r(&now, &ctx->local);
    ctx->local_day = local_day(&ctx->local);
    ctx->radar = app_radar_ui(); /* M6 */
    ctx->solar = app_solar_ui(); /* M6d */
    ctx->pc_ids = s_presets.pc_ids; /* M7: the PC readings the presets name */
    ctx->pc = app_pc_ui();
    ctx->history = app_pc_history();
    ctx->now_ms = (uint32_t)app_uptime_ms();

    lora_stats_t st = lora_widget_get_stats();
    memset(&s_lora_ui, 0, sizeof(s_lora_ui));
    s_lora_ui.enabled = st.enabled;
    s_lora_ui.initialized = st.initialized;
    s_lora_ui.ble_connected = st.ble_connected;
    s_lora_ui.ble_pin = st.ble_pin;
    s_lora_ui.freq_khz = st.freq_khz;
    s_lora_ui.bw_hz = st.bw_hz;
    s_lora_ui.sf = st.sf;
    s_lora_ui.cr = st.cr;
    s_lora_ui.tx_power_dbm = st.tx_power_dbm;
    snprintf(s_lora_ui.node_name, sizeof(s_lora_ui.node_name), "%.*s",
             (int)(sizeof(s_lora_ui.node_name) - 1), st.node_name);
    snprintf(s_lora_ui.channel_name, sizeof(s_lora_ui.channel_name), "%.*s",
             (int)(sizeof(s_lora_ui.channel_name) - 1), st.channel_name);
    s_lora_ui.packet_count = st.packet_count;
    s_lora_ui.packets_sent = st.packets_sent;
    s_lora_ui.sent_flood = st.sent_flood;
    s_lora_ui.last_rssi_dbm = st.last_rssi_dbm;
    s_lora_ui.last_snr_q4 = st.last_snr_q4;
    s_lora_ui.noise_floor_dbm = st.noise_floor_dbm;

    size_t total_msgs = st.msg_count;
    size_t start_msg_idx = (total_msgs > UI_LORA_MAX_MSGS) ? (total_msgs - UI_LORA_MAX_MSGS) : 0;
    s_lora_ui.total_msg_count = (int)total_msgs;
    s_lora_ui.msg_count = 0;
    for (size_t i = start_msg_idx; i < total_msgs && s_lora_ui.msg_count < UI_LORA_MAX_MSGS; i++) {
        meshcore_msg_t msg;
        if (!lora_widget_get_message(i, &msg)) {
            break;
        }
        ui_lora_msg_t *dst = &s_lora_ui.msgs[s_lora_ui.msg_count++];
        dst->timestamp = msg.timestamp;
        snprintf(dst->text, sizeof(dst->text), "%.*s",
                 (int)(sizeof(dst->text) - 1), msg.text);
        dst->snr_q4 = msg.snr_q4;
        dst->hop_count = msg.hops;
        dst->outgoing = msg.outgoing;
        dst->is_direct = false;
        dst->sender[0] = '\0';
    }

    meshcore_advert_t all_advs[MESHCORE_MAX_ADVERTS];
    size_t adv_count = 0;
    for (size_t i = 0; i < MESHCORE_MAX_ADVERTS; i++) {
        if (lora_widget_get_advert(i, &all_advs[adv_count])) {
            adv_count++;
        }
    }
    s_lora_ui.total_node_count = (int)adv_count;

    /* Sort adverts descending by last_seen */
    for (size_t i = 0; i < adv_count; i++) {
        for (size_t j = i + 1; j < adv_count; j++) {
            uint32_t time_i = all_advs[i].rx_time_s ? all_advs[i].rx_time_s : all_advs[i].timestamp;
            uint32_t time_j = all_advs[j].rx_time_s ? all_advs[j].rx_time_s : all_advs[j].timestamp;
            if (time_j > time_i) {
                meshcore_advert_t tmp = all_advs[i];
                all_advs[i] = all_advs[j];
                all_advs[j] = tmp;
            }
        }
    }

    s_lora_ui.node_count = 0;
    for (size_t i = 0; i < adv_count && s_lora_ui.node_count < UI_LORA_MAX_NODES; i++) {
        const meshcore_advert_t *adv = &all_advs[i];
        ui_lora_node_t *dst = &s_lora_ui.nodes[s_lora_ui.node_count++];
        snprintf(dst->name, sizeof(dst->name), "%.*s",
                 (int)(sizeof(dst->name) - 1), adv->name);
        memcpy(dst->pub_key_prefix, adv->pub_key, 4);
        dst->node_type = adv->node_type;
        dst->last_rssi_dbm = adv->rssi_dbm;
        dst->last_snr_q4 = adv->snr_q4;
        dst->hops = adv->hops;
        dst->last_seen = adv->rx_time_s ? adv->rx_time_s : adv->timestamp;
    }

    if (s_lora_ui.node_count == 0) {
        meshcore_neighbor_t all_nbrs[16];
        size_t nbr_count = 0;
        for (size_t i = 0; i < 16; i++) {
            if (lora_widget_get_neighbor(i, &all_nbrs[nbr_count])) {
                nbr_count++;
            }
        }
        s_lora_ui.total_node_count = (int)nbr_count;
        for (size_t i = 0; i < nbr_count && s_lora_ui.node_count < UI_LORA_MAX_NODES; i++) {
            const meshcore_neighbor_t *nbr = &all_nbrs[i];
            ui_lora_node_t *dst = &s_lora_ui.nodes[s_lora_ui.node_count++];
            snprintf(dst->name, sizeof(dst->name), "%.*s",
                     (int)(sizeof(dst->name) - 1), nbr->name);
            memcpy(dst->pub_key_prefix, nbr->pub_key_prefix, 4);
            dst->node_type = nbr->node_type;
            dst->last_rssi_dbm = nbr->rssi_dbm;
            dst->last_snr_q4 = nbr->snr_out_q4;
            dst->hops = 0;
            dst->last_seen = nbr->rx_time_s;
        }
    }
    ctx->lora = &s_lora_ui;
}

void app_ui_render(void)
{
    gfx_fb_t *fb = display_fb();
    const ui_preset_t *active = &s_presets.presets[s_presets.active];
    bool flights = fb != NULL && !display_asleep() && !app_menu_is_open() && !s.critical &&
                   !app_config_shows_setup() && !s.first_run && active->layout == UI_LAYOUT_FLIGHTS;
    app_flights_tick(flights); /* spec §11.3: it polls only while the Flights view shows */
    if (fb == NULL || display_asleep()) {
        return; /* night sleep: nothing is drawn (spec §9.1) */
    }
    if (app_menu_is_open()) {
        app_menu_render();
        return;
    }
    app_radar_prepare(active);
    ui_context_t ctx;
    app_ui_context(&ctx);
    if (s.critical) {
        ui_draw_critical(fb, &ctx);
    } else if (app_config_shows_setup()) { /* while a phone is logged in, the dashboard (D20) */
        app_config_draw(fb, ctx.lang);
    } else if (s.first_run) {
        ui_draw_first_run(fb, &ctx);
    } else {
        ui_titles_t titles; /* the preset's own (spec §5.4, D42) */
        ui_presets_titles(&s_presets, s_presets.active, &titles);
        ui_draw_dashboard(fb, &ctx, &s_presets.presets[s_presets.active], &titles);
    }
    if (app_ui_toast_active()) {
        ui_draw_toast(fb, s_toast);
    }
    esp_err_t err = display_commit(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display: %s", esp_err_to_name(err));
    }
}

void app_ui_seed_previous(uint8_t partial_count)
{
    gfx_fb_t *fb = display_fb();
    if (fb == NULL || s.done_slot == 0 || s.critical || s.first_run) {
        return;
    }
    ui_context_t ctx;
    app_ui_context(&ctx);
    ctx.now = s.done_slot;
    localtime_r(&ctx.now, &ctx.local);
    ui_titles_t titles;
    ui_presets_titles(&s_presets, s_presets.active, &titles);
    ui_draw_dashboard(fb, &ctx, &s_presets.presets[s_presets.active], &titles);
    display_seed_previous(partial_count);
}

void app_ui_toast_arm(const char *text)
{
    snprintf(s_toast, sizeof(s_toast), "%s", text);
    s_toast_until_ms = app_uptime_ms() + TOAST_MS;
    power_hold_awake_ms(TOAST_MS); /* a sleep would leave it on screen until the next wake */
    ESP_LOGI(TAG, "toast: %s", text);
}

void app_ui_toast(const char *text)
{
    app_ui_toast_arm(text);
    app_ui_render();
}

bool app_ui_toast_active(void)
{
    return s_toast[0] != '\0' && app_uptime_ms() < s_toast_until_ms;
}

int64_t app_ui_toast_until_ms(void)
{
    return s_toast[0] != '\0' ? s_toast_until_ms : 0;
}

void app_ui_toast_expire(void)
{
    if (s_toast[0] != '\0' && !app_ui_toast_active()) {
        s_toast[0] = '\0';
        app_ui_render();
    }
}

static ds_bat_state_t ds_battery_state(battery_state_t state)
{
    switch (state) {
    case BATTERY_DISCHARGING:
        return DS_BAT_DISCHARGING;
    case BATTERY_CHARGING:
        return DS_BAT_CHARGING;
    case BATTERY_FULL:
        return DS_BAT_FULL;
    default:
        return DS_BAT_UNKNOWN;
    }
}

static bool parse_learning(const char *text, void *ctx)
{
    static uint8_t raw[BATTERY_LEARN_PACKED_MAX];
    int n = util_base64_decode(text, raw, sizeof(raw));
    return n > 0 && battery_learn_unpack(ctx, raw, (size_t)n);
}

void app_ui_restore_learning(void)
{
    static battery_learn_t l;
    static char text[LEARN_TEXT_MAX + 1];
    bool from_backup = false;
    if (storage_ready() && storage_load(LEARN_PATH, text, sizeof(text), parse_learning, &l, &from_backup) == ESP_OK) {
        sensors_learn_restore(&l);
        ESP_LOGI(TAG, "battery learning resumed: %s, %u h recorded", battery_learn_state_name(battery_learn_state(&l)),
                 (unsigned)battery_learn_hours(&l));
    }
}

/* The session into LittleFS when it changed (a point, a state); routine wakes without storage
 * leave it for the next time storage is up. */
static void save_learning(void)
{
    if (!storage_ready() || !sensors_learn_take_changed()) {
        return;
    }
    const battery_learn_t *l = sensors_learn();
    if (battery_learn_state(l) == BATTERY_LEARN_OFF) {
        remove(LEARN_PATH);
        remove(LEARN_PATH ".bak");
        return;
    }
    static uint8_t raw[BATTERY_LEARN_PACKED_MAX];
    static char text[LEARN_TEXT_MAX];
    size_t n = battery_learn_pack(l, raw, sizeof(raw));
    if (n == 0 || !util_base64_encode(raw, n, text, sizeof(text))) {
        return;
    }
    esp_err_t err = storage_write_atomic(LEARN_PATH, text, strlen(text));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery learning not saved: %s", esp_err_to_name(err));
    }
}

typedef struct {
    util_snapshot_hdr_t hdr;
    ds_weather_t weather;
    ds_air_t air;
} forecast_file_t;

void app_ui_save_forecast(void)
{
    static forecast_file_t f;
    if (storage_init() != ESP_OK) {
        return;
    }
    const ds_weather_t *w = ds_weather(&s.ds);
    const ds_air_t *a = ds_air(&s.ds);
    memset(&f, 0, sizeof(f));
    if (w != NULL) {
        f.weather = *w;
    }
    if (a != NULL) {
        f.air = *a;
    }
    util_snapshot_seal(&f, sizeof(f), FORECAST_MAGIC, FORECAST_VERSION);
    esp_err_t err = storage_write_atomic(FORECAST_PATH, (const char *)&f, sizeof(f));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "forecast not saved: %s", esp_err_to_name(err));
    }
    app_seal_snapshot();
}

void app_ui_restore_forecast(void)
{
    static forecast_file_t f;
    if (!storage_ready()) {
        return;
    }
    FILE *file = fopen(FORECAST_PATH, "rb");
    if (file == NULL) {
        return;
    }
    size_t n = fread(&f, 1, sizeof(f), file);
    fclose(file);
    if (n != sizeof(f) || !util_snapshot_valid(&f, sizeof(f), FORECAST_MAGIC, FORECAST_VERSION)) {
        ESP_LOGW(TAG, "%s: not a forecast of this firmware; left out", FORECAST_PATH);
        return;
    }
    if (f.weather.fetched != 0) {
        ds_set_weather(&s.ds, &f.weather);
    }
    if (f.air.fetched != 0) {
        ds_set_air(&s.ds, &f.air);
    }
    ESP_LOGI(TAG, "forecast from %lu restored", (unsigned long)f.weather.fetched);
}

void app_ui_learn(bool start)
{
    if (start) {
        sensors_learn_start();
    } else {
        sensors_learn_stop();
    }
    ESP_LOGI(TAG, "battery learning %s", start ? "armed: waiting for a charge" : "stopped");
    save_learning();
}

void app_ui_sample(time_t now)
{
    struct tm local;
    localtime_r(&now, &local);
    esp_err_t err = sensors_sample_env(now);
    if (err == ESP_OK) {
        sensors_env_t env = sensors_env();
        ds_set_env(&s.ds, env.temp_c100, env.hum_pct100, env.time, local_day(&local));
        display_set_ambient_c((int16_t)(env.temp_c100 / 100));
        timekeeping_set_ambient_c100((int16_t)env.temp_c100);
    } else {
        ESP_LOGW(TAG, "AHT20: %s; keeping the last reading", esp_err_to_name(err));
    }
    /* Config mode too (D20): the radio's load pulls VBAT down, which errs on the safe side, as a
     * battery that sags under it would brown out anyway. */
    err = sensors_sample_battery(now);
    if (err == ESP_OK) {
        sensors_battery_t bat = sensors_battery(now);
        lora_widget_set_battery((uint16_t)(bat.smoothed_mv > 0 ? bat.smoothed_mv : 0), (uint8_t)(bat.level >= 0 ? bat.level : 0));
        ds_set_battery(&s.ds, bat.level, bat.smoothed_mv, ds_battery_state(bat.state), now);
        int days10 = sensors_battery_days_left10(now);
        if (days10 >= 0) {
            ds_set(&s.ds, DS_BAT_DAYS, days10, now);
        } else {
            ds_clear(&s.ds, DS_BAT_DAYS);
        }
        bool critical = battery_critical(s.critical, bat.smoothed_mv, bat.state); /* spec §8 */
        if (critical && !s.critical) {
            ESP_LOGW(TAG, "battery critical: %d mV", bat.smoothed_mv);
        } else if (!critical && s.critical) {
            ESP_LOGI(TAG, "battery recovered: %d mV", bat.smoothed_mv);
        }
        s.critical = critical;
        if (critical) {
            app_pc_end("critical battery"); /* spec §12a.4 */
        }
        uint16_t curve[BATTERY_CURVE_POINTS];
        if (sensors_learn_take_curve(curve)) { /* D21: a full discharge reached its end */
            memcpy(s.settings.bat_learned_mv, curve, sizeof(curve));
            s.settings.bat_learned_at = (uint32_t)now;
            s.settings.bat_cal = SETTINGS_BAT_LEARNED;
            app_ui_save_settings();
            app_ui_apply_settings();
            ESP_LOGI(TAG, "battery curve learned: %u mV at 0 %%, %u mV at 50 %%, %u mV at 100 %%", curve[0], curve[10],
                     curve[BATTERY_CURVE_POINTS - 1]);
        }
        save_learning();
    } else {
        ESP_LOGW(TAG, "battery: %s", esp_err_to_name(err));
    }
    ds_take_changes(&s.ds); /* every sample is followed by a render anyway */
    app_pc_feed(true);      /* M7: the indoor values' charts, at each sample */
}

size_t app_ui_saved_presets_json(char *out, size_t size)
{
    uint8_t shown = s_presets.active; /* a PC session's own preset isn't saved: the one before it is (spec §12a.4) */
    const char *before = app_pc_saved_preset();
    int i = before != NULL ? ui_presets_find(&s_presets, before) : -1;
    s_presets.active = i >= 0 ? (uint8_t)i : shown;
    size_t n = ui_presets_to_json(&s_presets, out, size);
    s_presets.active = shown;
    return n;
}

esp_err_t app_ui_save_presets(void)
{
    size_t n = app_ui_saved_presets_json(s_file, sizeof(s_file));
    if (n == 0) {
        ESP_LOGE(TAG, "presets.json does not fit %u bytes", (unsigned)sizeof(s_file));
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = storage_init(); /* not mounted yet after a routine deep-sleep wake */
    if (err == ESP_OK) {
        err = storage_write_atomic(STORAGE_PRESETS_PATH, s_file, n);
    }
    if (err == ESP_OK) {
        app_seal_snapshot();
    } else {
        ESP_LOGE(TAG, "saving presets: %s", esp_err_to_name(err));
    }
    return err;
}

static bool keep_text(const char *text, void *ctx)
{
    (void)text;
    (void)ctx;
    return true; /* storage_load left it in s_settings_base */
}

esp_err_t app_ui_save_settings(void)
{
    app_pc_settings_saved(s.settings.sync_mode); /* before the save: a session's end gives back its preset */
    esp_err_t err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving settings: %s", esp_err_to_name(err));
        return err;
    }
    if (s_settings_base[0] == '\0') { /* a deep-sleep wake doesn't read the file: read it now */
        bool from_backup = false;
        if (storage_load(STORAGE_SETTINGS_PATH, s_settings_base, sizeof(s_settings_base), keep_text, NULL,
                         &from_backup) != ESP_OK) {
            s_settings_base[0] = '\0';
        }
    }
    EXT_RAM_BSS_ATTR static char out[sizeof(s_settings_base)];
    size_t n = app_ui_saved_settings_json(out, sizeof(out));
    if (n == 0) {
        ESP_LOGE(TAG, "settings.json does not fit %u bytes", (unsigned)sizeof(out));
        return ESP_ERR_INVALID_SIZE;
    }
    err = storage_write_atomic(STORAGE_SETTINGS_PATH, out, n);
    if (err == ESP_OK) {
        memcpy(s_settings_base, out, n + 1); /* the next save builds on what is now in the file */
        app_seal_snapshot();
    } else {
        ESP_LOGE(TAG, "saving settings: %s", esp_err_to_name(err));
    }
    return err;
}

static void focus_sync_on_app(void *arg)
{
    (void)arg;
    s_focus_sync_queued = false;
    bool is_focus = app_focus_mode_active();
    if (is_focus != s_focus_synced) {
        s_focus_synced = is_focus;
        app_focus_mode_sync(is_focus);
    }
}

void app_ui_select(int index, bool persist)
{
    if (index < 0 || index >= s_presets.count) {
        return;
    }
    s_presets.active = (uint8_t)index;
    bool is_focus = app_focus_mode_active();
    if (s_presets.cycle_enabled && !is_focus) {
        s.cycle_at = time(NULL) + s_presets.cycle_interval_s; /* a switch restarts the interval */
    }
    ESP_LOGI(TAG, "preset %s", s_presets.presets[index].id);
    app_ui_render();
    if (is_focus != s_focus_synced && !s_focus_sync_queued) {
        s_focus_sync_queued = true;
        if (app_post(focus_sync_on_app, NULL) != ESP_OK) {
            s_focus_sync_queued = false;
            s_focus_synced = is_focus;
            app_focus_mode_sync(is_focus);
        }
    }
    if (persist) {
        app_ui_save_presets();
    }
}

void app_ui_set_cycle(bool enabled)
{
    s_presets.cycle_enabled = enabled;
    s.cycle_at = enabled ? time(NULL) + s_presets.cycle_interval_s : 0;
    ESP_LOGI(TAG, "auto-cycle %s", enabled ? "on" : "off");
    app_ui_save_presets();
}

void app_ui_toggle_cycle(void)
{
    app_ui_set_cycle(!s_presets.cycle_enabled);
}

void app_ui_begin_night(time_t until)
{
    struct tm local;
    localtime_r(&until, &local);
    char text[48];
    snprintf(text, sizeof(text), "%s %02d:%02d", lang_str(lang_get(s.settings.language), LS_T_NIGHT_UNTIL),
             local.tm_hour, local.tm_min);
    app_ui_start_night(until);
    app_ui_toast(text);
}

void app_ui_start_night(time_t until)
{
    app_pc_end("night"); /* the console's night: what the session changed comes back first */
    s.night_until = until - until % 60;
    ESP_LOGI(TAG, "night until %lld", (long long)s.night_until);
}

void app_ui_end_night(void)
{
    s.sched_checked = ui_schedule_after_night(s.night_until); /* the entries inside it don't run */
    s.night_until = 0;
    ESP_LOGI(TAG, "night over");
}

bool app_ui_night(void)
{
    return s.night_until != 0;
}

static bool schedule_runs(void)
{
    return s_presets.schedule.enabled && s_presets.schedule.count > 0 && timekeeping_valid() && s.night_until == 0 &&
           !s.critical; /* the critical screen stays put, and a night would take its place */
}

/* Runs the entries that came due since the last check, in order; a night entry ends the run, as
 * the entries after it fall inside the night. */
static void run_schedule(time_t now)
{
    if (!schedule_runs()) {
        s.sched_checked = now;
        return;
    }
    /* Checks come at every display slot and at every entry's minute (spec §9.2): a longer gap was a
     * sleep no entry could end, such as the critical one. */
    time_t max_gap = (time_t)s.settings.display_every_min * 60 + 300;
    int order[UI_SCHEDULE_MAX];
    int n = ui_schedule_step(&s_presets.schedule, &s.sched_checked, now, max_gap, order);
    for (int i = 0; i < n; i++) {
        const ui_schedule_entry_t *e = &s_presets.schedule.entries[order[i]];
        if (e->action == UI_SCHED_NIGHT && app_pc_session()) {
            ESP_LOGI(TAG, "schedule: the night waits for the PC session"); /* spec §12a.4 */
            app_pc_defer_night(sched_next_weekly(now, e->until_min, 0x7F));
            continue;
        }
        if (e->action == UI_SCHED_NIGHT) {
            app_ui_begin_night(sched_next_weekly(now, e->until_min, 0x7F));
            break;
        }
        ESP_LOGI(TAG, "schedule: preset %s", s_presets.presets[e->preset].id);
        app_ui_select(e->preset, false);
    }
}

time_t app_ui_tick(bool force)
{
    time_t now = time(NULL);
    if (s.night_until != 0 && now < s.night_until && display_asleep()) {
        return now; /* night sleep: nothing is sampled or drawn (spec §9.1) */
    }
    bool focus = app_focus_mode_active();
    time_t slot = now - now % 60;
    bool new_slot = slot != s.done_slot;
    bool render = force || (DISPLAY_SUPPORTS_SECONDS && !focus && s_presets.presets[s_presets.active].seconds);
    int sensors_every = focus ? 15 : s.settings.sensors_every_min;
    int display_every = focus ? 1 : s.settings.display_every_min;
    if (force || s.done_slot == 0 || !sensors_battery(now).valid || (new_slot && scheduler_is_slot(slot, sensors_every))) {
        app_ui_sample(now);
        render = true;
    }
    if (new_slot && scheduler_is_slot(slot, display_every)) {
        render = true;
    }
    s.done_slot = slot;
    app_sync_tick();
    if (!focus) {
        run_schedule(now);
        if (s_presets.cycle_enabled && (s.cycle_at == 0 || s.cycle_at - now > s_presets.cycle_interval_s)) {
            s.cycle_at = now + s_presets.cycle_interval_s; /* the first tick, or the clock moved back */
        }
        if (s_presets.cycle_enabled && now >= s.cycle_at && app_pc_pauses_cycle()) {
            s.cycle_at = now + s_presets.cycle_interval_s; /* spec §12a.4: it waits while the session's preset shows */
        }
        if (s_presets.cycle_enabled && now >= s.cycle_at) {
            time_t at = timekeeping_valid() ? now : 0; /* without a valid clock every window is open (spec §5.4) */
            app_ui_select(ui_presets_cycle_next_with_lora(&s_presets, s.settings.sync_mode == SETTINGS_SYNC_ALWAYS,
                                                          s.settings.lora.enabled, at,
                                                          s.settings.lat_e4, s.settings.lon_e4),
                          false); /* renders; not saved, the cycle will move on */
            app_seal_snapshot();
            return now;
        }
        if (s.settings.lora.enabled && s_presets.presets[s_presets.active].layout == UI_LAYOUT_LORA) {
            lora_stats_t st = lora_widget_get_stats();
            if (st.msg_seq != s_last_lora_msg_seq ||
                st.msg_count != s_last_lora_msg_count ||
                st.advert_seq != s_last_lora_adv_seq ||
                st.ble_connected != s_last_lora_ble) {
                s_last_lora_msg_seq = st.msg_seq;
                s_last_lora_msg_count = st.msg_count;
                s_last_lora_adv_seq = st.advert_seq;
                s_last_lora_ble = st.ble_connected;
                s_last_lora_pkt_count = st.packet_count;
                s_last_lora_sent_count = st.packets_sent;
                render = true;
            }
        }
    }
    if (render) {
        app_ui_render();
    }
    app_seal_snapshot();
    return now;
}

sched_wake_t app_ui_next_wake(time_t now)
{
    int index = -1;
    bool focus = app_focus_mode_active();
    sched_input_t in = {
        .now = now,
        .display_every_min = focus ? 1 : s.settings.display_every_min,
        .sensors_every_min = focus ? 15 : s.settings.sensors_every_min,
        .cycle_at = (s_presets.cycle_enabled && !focus) ? s.cycle_at : 0,
        .every_second = DISPLAY_SUPPORTS_SECONDS && !focus && s_presets.presets[s_presets.active].seconds,
        .schedule_at = (schedule_runs() && !focus) ? ui_schedule_next(&s_presets.schedule, now, &index) : 0,
        .sync_at = (s.critical || s.night_until != 0) ? 0 : app_sync_due(),
    };
    return scheduler_next_wake(&in);
}

bool app_ui_first_run(void)
{
    return s.first_run;
}

void app_ui_end_first_run(void)
{
    if (!s.first_run) {
        return;
    }
    s.first_run = false;
    app_ui_save_settings(); /* the file now exists, so the first run doesn't come back */
    app_ui_render();
}

size_t app_ui_settings_json(char *out, size_t size)
{
    return settings_to_json(&s.settings, s_settings_base[0] ? s_settings_base : NULL, out, size);
}

size_t app_ui_saved_settings_json(char *out, size_t size)
{
    static settings_t saved; /* a PC session's `always` isn't saved: the mode before it is (spec §12a.4) */
    saved = s.settings;
    saved.sync_mode = app_pc_saved_mode(saved.sync_mode);
    return settings_to_json(&saved, s_settings_base[0] ? s_settings_base : NULL, out, size);
}

/* The new settings take effect everywhere: time zone, offsets, panel rate, slots and the text. */
static void settings_changed(void)
{
    app_ui_apply_settings();
    app_ui_sample(time(NULL));
    app_clock_moved(0); /* new slots or a new zone: schedule again, and redraw */
}

esp_err_t app_ui_replace_settings(const char *json, char *err, size_t err_size)
{
    static settings_t parsed;
    if (!settings_from_json(json, &s.settings, &parsed, err, err_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(json);
    if (n >= sizeof(s_settings_base)) {
        snprintf(err, err_size, "settings.json is larger than %u bytes", (unsigned)sizeof(s_settings_base) - 1);
        return ESP_ERR_INVALID_SIZE;
    }
    settings_replaced(&parsed, &s.settings); /* a page that turns `always` on: BOOT double returns */
    s.settings = parsed;
    memcpy(s_settings_base, json, n + 1); /* keys this firmware doesn't know stay, as in the file */
    esp_err_t e = app_ui_save_settings();
    settings_changed();
    return e;
}

esp_err_t app_ui_patch_settings(const char *patch, char *err, size_t err_size)
{
    EXT_RAM_BSS_ATTR static char merged[sizeof(s_settings_base)];
    EXT_RAM_BSS_ATTR static char base[sizeof(s_settings_base)];
    if (app_ui_settings_json(base, sizeof(base)) == 0 ||
        settings_patch(base, patch, merged, sizeof(merged), err, err_size) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return app_ui_replace_settings(merged, err, err_size);
}

esp_err_t app_ui_replace_presets(const ui_presets_t *presets)
{
    uint8_t offered = s_presets.offered;
    s_presets = *presets;
    s_presets.offered |= offered; /* a page or backup without the marker: what was offered stays offered */
    bool is_focus = app_focus_mode_active();
    if (is_focus != s_focus_synced) {
        s_focus_synced = is_focus;
        app_focus_mode_sync(is_focus);
    }
    s.cycle_at = 0;              /* the next tick starts the cycle interval */
    s.sched_checked = time(NULL); /* entries don't run late for a new schedule */
    esp_err_t err = app_ui_save_presets();
    app_pc_presets_changed(); /* M7: their charts' series */
    app_ui_render();
    return err;
}

void app_ui_set_zone(const char *iana, const char *posix)
{
    snprintf(s.settings.tz_iana, sizeof(s.settings.tz_iana), "%s", iana);
    snprintf(s.settings.tz_posix, sizeof(s.settings.tz_posix), "%s", posix);
    app_ui_save_settings();
    settings_changed();
}
