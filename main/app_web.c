#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_internal.h"
#include "cJSON.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "lora_widget.h"
#include "meshcore_companion.h"
#include "netmgr.h"
#include "sensors.h"
#include "storage_backup.h"
#include "timekeeping.h"
#include "ui_catalog.h"
#include "ui_dashboard.h"
#include "util_time.h"
#include "webui.h"

/* The web configurator's API (spec §10.3), apart from what webui answers itself (the password,
 * Wi-Fi, OTA and the system actions). Every call runs on the app task, which owns this state. */

static const char *TAG = "app_web";

#define MIN_EPOCH 1577836800 /* 2020-01-01: a phone's clock is at least this */
#define MAX_EPOCH              4102444799 /* 2099-12-31 23:59:59: the RTC's last second */

static void reply_error(webui_reply_t *reply, uint8_t *out, size_t size, int status, const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    reply->status = cJSON_PrintPreallocated(o, (char *)out, (int)size, false) ? status : 500;
    reply->len = reply->status == status ? strlen((char *)out) : 0;
    reply->type = "application/json";
    cJSON_Delete(o);
}

/* Prints `o` into the reply and frees it. */
static void reply_cjson(webui_reply_t *reply, uint8_t *out, size_t size, cJSON *o)
{
    if (cJSON_PrintPreallocated(o, (char *)out, (int)size, false)) {
        reply->status = 200;
        reply->len = strlen((char *)out);
        reply->type = "application/json";
    } else {
        reply_error(reply, out, size, 500, "the reply doesn't fit");
    }
    cJSON_Delete(o);
}

/* A JSON text written by one of the codecs; 0 bytes means it didn't fit. */
static void reply_text(webui_reply_t *reply, uint8_t *out, size_t size, size_t len)
{
    if (len == 0) {
        reply_error(reply, out, size, 500, "the reply doesn't fit");
        return;
    }
    reply->status = 200;
    reply->len = len;
    reply->type = "application/json";
}

static const char *battery_state_name(uint8_t state)
{
    static const char *const k_names[] = { "unknown", "discharging", "charging", "full" };
    return state < sizeof(k_names) / sizeof(k_names[0]) ? k_names[state] : "unknown";
}

static const char *net_state_name(netmgr_state_t state)
{
    switch (state) {
    case NETMGR_JOINING:
        return "joining";
    case NETMGR_STATION:
        return "station";
    case NETMGR_AP:
        return "ap";
    default:
        return "off";
    }
}

/* A datastore value in its unit (0.01 °C, 0.01 %, 0.1 d) as a JSON number, or null. */
static void add_value(cJSON *o, const char *key, ds_field_t field, double scale)
{
    ds_entry_t e;
    if (ds_get(app_ds(), field, &e) && e.updated != 0) {
        cJSON_AddNumberToObject(o, key, e.value / scale);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

static void get_status(uint8_t *out, size_t size, webui_reply_t *reply)
{
    const app_ui_state_t *st = app_state();
    const esp_app_desc_t *app = esp_app_get_description();
    netmgr_status_t net;
    netmgr_status(&net);
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char text[40];
    cJSON *o = cJSON_CreateObject();

    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON_AddStringToObject(dev, "id", net.host);
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(dev, "mac", text);
    cJSON_AddStringToObject(dev, "firmware", app->version);
    cJSON_AddNumberToObject(dev, "uptime_s", (double)(app_uptime_ms() / 1000));
    cJSON_AddNumberToObject(dev, "free_heap", esp_get_free_heap_size());
    cJSON_AddStringToObject(dev, "language", st->settings.language);

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    cJSON *t = cJSON_AddObjectToObject(o, "time");
    cJSON_AddBoolToObject(t, "valid", timekeeping_valid());
    cJSON_AddNumberToObject(t, "epoch", (double)now);
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    cJSON_AddStringToObject(t, "local", text);
    cJSON_AddStringToObject(t, "tz_iana", st->settings.tz_iana);
    cJSON_AddStringToObject(t, "tz_posix", st->settings.tz_posix);

    cJSON *bat = cJSON_AddObjectToObject(o, "battery");
    ds_entry_t e;
    if (ds_get(app_ds(), DS_BAT_LEVEL, &e) && e.updated != 0) {
        cJSON_AddNumberToObject(bat, "percent", e.value);
        cJSON_AddNumberToObject(bat, "mv", e.mv);
        cJSON_AddStringToObject(bat, "state", battery_state_name(e.bat_state));
    }
    add_value(bat, "days_left", DS_BAT_DAYS, 10.0);
    cJSON_AddBoolToObject(bat, "critical", st->critical);
    const battery_learn_t *l = sensors_learn(); /* D21 */
    cJSON *learn = cJSON_AddObjectToObject(bat, "learn");
    cJSON_AddStringToObject(learn, "state", battery_learn_state_name(battery_learn_state(l)));
    cJSON_AddNumberToObject(learn, "hours", battery_learn_hours(l));

    cJSON *env = cJSON_AddObjectToObject(o, "sensors");
    add_value(env, "temp_c", DS_ENV_TEMP, 100.0);
    add_value(env, "hum_pct", DS_ENV_HUM, 100.0);
    if (ds_get(app_ds(), DS_ENV_TEMP, &e) && e.updated != 0 && now >= (time_t)e.updated) {
        cJSON_AddNumberToObject(env, "age_s", (double)(now - (time_t)e.updated));
    }

    cJSON *wifi = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(wifi, "state", net_state_name(net.state));
    cJSON_AddStringToObject(wifi, "ssid", net.ssid);
    cJSON_AddStringToObject(wifi, "ip", net.ip);
    cJSON_AddNumberToObject(wifi, "rssi", net.rssi);
    cJSON_AddBoolToObject(wifi, "ap_on", net.ap_on);
    cJSON_AddStringToObject(wifi, "ap_ssid", net.ap_ssid);
    cJSON_AddNumberToObject(wifi, "ap_clients", net.ap_clients);

    /* spec §10.3, M5: the sync, the RTC's trim and the forecast's age */
    cJSON *sync = cJSON_AddObjectToObject(o, "sync");
    static const char *const k_modes[] = { "times", "interval", "always", "manual" };
    cJSON_AddStringToObject(sync, "mode", k_modes[st->settings.sync_mode <= SETTINGS_SYNC_MANUAL
                                                       ? st->settings.sync_mode : 0]);
    bool running = app_sync_running(); /* a radar refresh isn't a sync (spec §9.3) */
    cJSON_AddBoolToObject(sync, "running", running);
    if (running) {
        cJSON_AddStringToObject(sync, "step", sync_step_name(sync_step()));
    }
    if (st->sync.last_at != 0) {
        cJSON *last = cJSON_AddObjectToObject(sync, "last");
        cJSON_AddNumberToObject(last, "at", st->sync.last_at);
        cJSON *steps = cJSON_AddObjectToObject(last, "steps");
        static const char *const k_results[] = { "skipped", "ok", "failed", "kept" };
        for (int i = 0; i < SYNC_STEP_COUNT; i++) {
            uint8_t r = st->sync.last_result[i];
            cJSON_AddStringToObject(steps, sync_step_name((sync_step_t)i), k_results[r <= SYNC_STEP_KEPT ? r : 0]);
        }
        cJSON *details = cJSON_AddObjectToObject(last, "details"); /* why a step failed, kept or was skipped */
        for (int i = 0; i < SYNC_STEP_COUNT; i++) {
            if (st->sync.last_detail[i][0] != '\0') {
                cJSON_AddStringToObject(details, sync_step_name((sync_step_t)i), st->sync.last_detail[i]);
            }
        }
        cJSON_AddBoolToObject(last, "ok", !sync_report_failed(st->sync.last_result)); /* the Energy step aside, D36 */
        if (st->sync.last_failed_step < SYNC_STEP_COUNT) {
            cJSON_AddStringToObject(last, "failed", sync_step_name((sync_step_t)st->sync.last_failed_step));
            cJSON_AddStringToObject(last, "detail", st->sync.last_detail[st->sync.last_failed_step]);
        }
    }
    if (st->sync.due.at != 0) {
        cJSON_AddNumberToObject(sync, "next", (double)st->sync.due.at);
        cJSON_AddBoolToObject(sync, "next_retry", st->sync.due.retry);
    }
    const ds_weather_t *w = ds_weather(app_ds());
    if (w != NULL) {
        cJSON_AddNumberToObject(sync, "weather_at", w->fetched);
    }
    const ds_air_t *a = ds_air(app_ds());
    if (a != NULL) {
        cJSON_AddNumberToObject(sync, "air_at", a->fetched);
    }
    const rtc_trim_t *trim = timekeeping_trim();
    cJSON *rtc = cJSON_AddObjectToObject(t, "rtc");
    cJSON_AddNumberToObject(rtc, "trim_steps", trim->offset);
    if (trim->drift_ppb != TRIM_NO_DRIFT) {
        cJSON_AddNumberToObject(rtc, "drift_s_per_day", timekeeping_trim_drift_s10_per_day(trim) / 10.0);
    }

    /* spec §10.3, M6: the weather radar's frames and the flight radar's polls */
    app_radar_status_t rs;
    app_radar_status(&rs);
    cJSON *radar = cJSON_AddObjectToObject(o, "radar");
    cJSON *weather = cJSON_AddObjectToObject(radar, "weather");
    cJSON_AddStringToObject(weather, "source", rs.source == RADAR_SOURCE_CHMU ? "chmu" : "rainviewer");
    cJSON_AddNumberToObject(weather, "frames", rs.frames);
    if (rs.frame_at != 0) {
        cJSON_AddNumberToObject(weather, "frame_at", rs.frame_at);
    }
    if (rs.fetched_at != 0) {
        cJSON_AddNumberToObject(weather, "fetched_at", (double)rs.fetched_at);
        if (!rs.ok) {
            cJSON_AddStringToObject(weather, "error", rs.detail);
        }
    }
    app_flights_status_t fs;
    app_flights_status(&fs);
    cJSON *flights = cJSON_AddObjectToObject(radar, "flights");
    cJSON_AddBoolToObject(flights, "on", fs.on);
    cJSON_AddNumberToObject(flights, "aircraft", fs.aircraft);
    if (fs.updated != 0) {
        cJSON_AddNumberToObject(flights, "updated", (double)fs.updated);
    }
    cJSON_AddBoolToObject(flights, "failed", fs.failed);
    if (fs.failed && fs.error[0] != '\0') {
        cJSON_AddStringToObject(flights, "error", fs.error);
    }
    if (fs.routes_paused_until != 0) {
        cJSON_AddNumberToObject(flights, "routes_paused_until", (double)fs.routes_paused_until);
    }

    /* spec §10.3, M6d: the PV forecast and the house's energy, never a key */
    const app_solar_state_t *ss = app_solar_state();
    static const char *const k_solar[] = { "off", "open-meteo", "forecast-solar", "solcast" };
    cJSON *solar = cJSON_AddObjectToObject(o, "solar");
    uint8_t src = st->settings.solar_source;
    cJSON_AddStringToObject(solar, "source", k_solar[src <= SETTINGS_SOLAR_SOLCAST ? src : 0]);
    if (ss->forecast.fetched != 0) {
        cJSON_AddNumberToObject(solar, "fetched_at", ss->forecast.fetched);
    }
    if (ss->forecast.day != 0) {
        int y, m, d;
        util_civil_from_days(ss->forecast.day, &y, &m, &d);
        char day[16];
        snprintf(day, sizeof(day), "%04d-%02d-%02d", y, m, d);
        cJSON_AddStringToObject(solar, "day", day); /* the local day of its first quarter hours */
    }
    if (ss->forecast_tried != 0) {
        cJSON_AddNumberToObject(solar, "tried_at", ss->forecast_tried);
    }
    if (ss->forecast_error[0] != '\0') {
        cJSON_AddStringToObject(solar, "error", ss->forecast_error); /* the last call's */
    }
    if (ss->forecast_kept[0] != '\0') {
        cJSON_AddStringToObject(solar, "kept", ss->forecast_kept);
    }
    if (src == SETTINGS_SOLAR_SOLCAST && ss->solcast_asked != 0) { /* when its budget lets the next step ask */
        cJSON_AddNumberToObject(solar, "next_at", (double)ss->solcast_asked + solar_solcast_wait_s(ss->solcast_sites));
    }
    if (ss->demo) {
        cJSON_AddBoolToObject(solar, "demo", true);
    }
    cJSON *energy = cJSON_AddObjectToObject(o, "energy");
    static const char *const k_energy[] = { "off", "solax", "solax-dev" };
    uint8_t esrc = st->settings.energy_source;
    cJSON_AddStringToObject(energy, "source", k_energy[esrc <= SETTINGS_ENERGY_SOLAX_DEV ? esrc : 0]);
    if (ss->reading.at != 0) {
        cJSON_AddNumberToObject(energy, "reading_at", ss->reading.at);
    }
    if (ss->energy_tried != 0) {
        cJSON_AddNumberToObject(energy, "tried_at", ss->energy_tried);
    }
    if (ss->energy_error[0] != '\0') {
        cJSON_AddStringToObject(energy, "error", ss->energy_error);
    }
    if (esrc == SETTINGS_ENERGY_SOLAX_DEV && ss->site.plant_id[0] != '\0') {
        cJSON_AddStringToObject(energy, "plant", ss->site.plant_id); /* the Developer API's, found once (D37) */
    }

    const ui_preset_t *active = &app_presets()->presets[app_presets()->active];
    cJSON *preset = cJSON_AddObjectToObject(o, "preset");
    cJSON_AddStringToObject(preset, "active", active->id);
    cJSON_AddStringToObject(preset, "name", active->name);
    cJSON_AddBoolToObject(preset, "cycle", app_presets()->cycle_enabled);

    lora_stats_t ls = lora_widget_get_stats();
    cJSON *lora = cJSON_AddObjectToObject(o, "lora");
    cJSON_AddBoolToObject(lora, "enabled", ls.enabled);
    cJSON_AddBoolToObject(lora, "initialized", ls.initialized);
    cJSON_AddNumberToObject(lora, "ble_pin", ls.ble_pin);
    cJSON_AddBoolToObject(lora, "ble_connected", ls.ble_connected);
    cJSON_AddStringToObject(lora, "node_name", ls.node_name);
    cJSON_AddStringToObject(lora, "pub_key", ls.pub_key_hex);
    cJSON_AddNumberToObject(lora, "freq_khz", ls.freq_khz);
    cJSON_AddNumberToObject(lora, "bw_hz", ls.bw_hz);
    cJSON_AddNumberToObject(lora, "bw_khz", (double)ls.bw_hz / 1000.0);
    cJSON_AddNumberToObject(lora, "sf", ls.sf);
    cJSON_AddNumberToObject(lora, "cr", ls.cr);
    cJSON_AddNumberToObject(lora, "tx_power_dbm", ls.tx_power_dbm);
    cJSON_AddNumberToObject(lora, "sync_word", ls.sync_word);
    cJSON_AddBoolToObject(lora, "rx_boosted", ls.rx_boosted);
    cJSON_AddBoolToObject(lora, "cad_enabled", ls.cad_enabled);
    cJSON_AddBoolToObject(lora, "client_repeat", ls.client_repeat);
    cJSON_AddNumberToObject(lora, "path_hash_mode", ls.path_hash_mode);
    cJSON_AddNumberToObject(lora, "airtime_factor", (double)ls.airtime_factor_x10 / 10.0);
    cJSON_AddBoolToObject(lora, "wake_on_rx", ls.wake_on_rx);
    cJSON_AddBoolToObject(lora, "share_loc", ls.share_loc);
    cJSON_AddStringToObject(lora, "channel_name", ls.channel_name);
    cJSON_AddNumberToObject(lora, "channel_hash", ls.channel_hash);
    cJSON_AddNumberToObject(lora, "noise_floor_dbm", ls.noise_floor_dbm);
    cJSON_AddNumberToObject(lora, "last_rssi_dbm", ls.last_rssi_dbm);
    cJSON_AddNumberToObject(lora, "last_snr_db", (double)ls.last_snr_q4 / 4.0);
    cJSON_AddNumberToObject(lora, "packet_count", ls.packet_count);
    cJSON_AddNumberToObject(lora, "packets_sent", ls.packets_sent);
    cJSON_AddNumberToObject(lora, "recv_flood", ls.recv_flood);
    cJSON_AddNumberToObject(lora, "recv_direct", ls.recv_direct);
    cJSON_AddNumberToObject(lora, "sent_flood", ls.sent_flood);
    cJSON_AddNumberToObject(lora, "sent_direct", ls.sent_direct);
    cJSON_AddNumberToObject(lora, "recv_errors", ls.recv_errors);
    cJSON_AddNumberToObject(lora, "tx_air_ms", ls.tx_air_ms);
    cJSON_AddNumberToObject(lora, "rx_air_ms", ls.rx_air_ms);
    cJSON_AddNumberToObject(lora, "last_packet_at", ls.last_packet_time_s);
    if (ls.last_packet_time_s != 0 && now >= (time_t)ls.last_packet_time_s) {
        cJSON_AddNumberToObject(lora, "age_s", (double)(now - (time_t)ls.last_packet_time_s));
    } else {
        cJSON_AddNullToObject(lora, "age_s");
    }

    cJSON *advs = cJSON_AddArrayToObject(lora, "adverts");
    for (size_t i = 0; i < ls.advert_count && i < MESHCORE_MAX_ADVERTS; i++) {
        meshcore_advert_t adv;
        if (!lora_widget_get_advert(i, &adv)) break;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", adv.name);
        cJSON_AddNumberToObject(item, "node_type", adv.node_type);
        char pk_hex[17];
        for (int j = 0; j < 8; j++) {
            snprintf(pk_hex + j * 2, 3, "%02x", adv.pub_key[j]);
        }
        cJSON_AddStringToObject(item, "pub_key", pk_hex);
        cJSON_AddNumberToObject(item, "hops", adv.hops);
        cJSON_AddNumberToObject(item, "rssi_dbm", adv.rssi_dbm);
        cJSON_AddNumberToObject(item, "snr_db", (double)adv.snr_q4 / 4.0);
        cJSON_AddBoolToObject(item, "has_loc", adv.has_loc);
        if (adv.has_loc) {
            cJSON_AddNumberToObject(item, "lat", (double)adv.lat_e6 / 1000000.0);
            cJSON_AddNumberToObject(item, "lon", (double)adv.lon_e6 / 1000000.0);
        } else {
            cJSON_AddNullToObject(item, "lat");
            cJSON_AddNullToObject(item, "lon");
        }
        if (adv.rx_time_s != 0 && now >= (time_t)adv.rx_time_s) {
            cJSON_AddNumberToObject(item, "age_s", (double)(now - (time_t)adv.rx_time_s));
        } else {
            cJSON_AddNullToObject(item, "age_s");
        }
        cJSON_AddItemToArray(advs, item);
    }

    cJSON *nbrs = cJSON_AddArrayToObject(lora, "neighbors");
    for (size_t i = 0; i < ls.neighbor_count && i < MESHCORE_MAX_NEIGHBORS; i++) {
        meshcore_neighbor_t nbr;
        if (!lora_widget_get_neighbor(i, &nbr)) break;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", nbr.name);
        cJSON_AddNumberToObject(item, "node_type", nbr.node_type);
        char pk_hex[17];
        for (int j = 0; j < 8; j++) {
            snprintf(pk_hex + j * 2, 3, "%02x", nbr.pub_key_prefix[j]);
        }
        cJSON_AddStringToObject(item, "pub_key", pk_hex);
        cJSON_AddNumberToObject(item, "snr_in_db", (double)nbr.snr_in_q4 / 4.0);
        cJSON_AddNumberToObject(item, "snr_out_db", (double)nbr.snr_out_q4 / 4.0);
        cJSON_AddNumberToObject(item, "rssi_dbm", nbr.rssi_dbm);
        if (nbr.rx_time_s != 0 && now >= (time_t)nbr.rx_time_s) {
            cJSON_AddNumberToObject(item, "age_s", (double)(now - (time_t)nbr.rx_time_s));
        } else {
            cJSON_AddNullToObject(item, "age_s");
        }
        cJSON_AddItemToArray(nbrs, item);
    }

    cJSON *msgs = cJSON_AddArrayToObject(lora, "messages");
    for (size_t i = 0; i < ls.msg_count && i < MESHCORE_MAX_MESSAGES; i++) {
        meshcore_msg_t m;
        if (!lora_widget_get_message(i, &m)) break;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "text", m.text);
        cJSON_AddNumberToObject(item, "rssi_dbm", m.rssi_dbm);
        cJSON_AddNumberToObject(item, "snr_db", (double)m.snr_q4 / 4.0);
        cJSON_AddNumberToObject(item, "hops", m.hops);
        cJSON_AddBoolToObject(item, "outgoing", m.outgoing);
        if (m.rx_time_s != 0 && now >= (time_t)m.rx_time_s) {
            cJSON_AddNumberToObject(item, "age_s", (double)(now - (time_t)m.rx_time_s));
        } else {
            cJSON_AddNullToObject(item, "age_s");
        }
        cJSON_AddItemToArray(msgs, item);
    }

    reply_cjson(reply, out, size, o);
}

/* The framebuffer the previews draw into; the display's own stays as it is. */
static gfx_fb_t *preview_fb(void)
{
    static gfx_fb_t fb;
    static uint8_t *buf;
    if (buf == NULL) {
        buf = heap_caps_malloc(gfx_fb_size(400, 300), MALLOC_CAP_SPIRAM);
        if (buf == NULL) {
            return NULL;
        }
        gfx_fb_init(&fb, buf, 400, 300);
    }
    return &fb;
}

static void reply_bmp(const gfx_fb_t *fb, uint8_t *out, size_t size, webui_reply_t *reply)
{
    size_t n = fb != NULL ? gfx_bmp_encode(fb, out, size) : 0;
    if (n == 0) {
        reply_error(reply, out, size, 500, "no image");
        return;
    }
    reply->status = 200;
    reply->len = n;
    reply->type = "image/bmp";
}

/* GET /api/preview.bmp?preset=<id>: a saved preset. POST: a presets document from the editor,
 * validated like presets.json, and ?preset=<id> in it (default: its active one). */
static void preview(const char *method, const char *query, const char *body, uint8_t *out, size_t size,
                    webui_reply_t *reply)
{
    EXT_RAM_BSS_ATTR static ui_presets_t doc; /* scratch buffers in PSRAM (AGENTS.md §8) */
    char id[UI_PRESET_ID_LEN] = "", err[UI_PRESET_ERR_LEN];
    const char *eq = strstr(query, "preset=");
    if (eq != NULL) {
        snprintf(id, sizeof(id), "%.*s", (int)strcspn(eq + 7, "&"), eq + 7);
    }
    if (strcmp(method, "POST") == 0) {
        if (!ui_presets_from_json(body, &doc, err, sizeof(err))) {
            reply_error(reply, out, size, 400, err);
            return;
        }
    } else {
        doc = *app_presets();
    }
    int index = id[0] ? ui_presets_find(&doc, id) : doc.active;
    if (index < 0) {
        reply_error(reply, out, size, 404, "no such preset");
        return;
    }
    gfx_fb_t *fb = preview_fb();
    if (fb != NULL) {
        app_radar_prepare(&doc.presets[index]); /* its map, and the frame from its file if PSRAM has none */
        ui_context_t ctx;
        app_ui_context(&ctx);
        ctx.pc_ids = doc.pc_ids; /* M7: the previewed file's own PC readings */
        ui_titles_t titles;
        ui_presets_titles(&doc, index, &titles);
        ui_draw_dashboard(fb, &ctx, &doc.presets[index], &titles);
    }
    reply_bmp(fb, out, size, reply);
}

static bool json_number(const cJSON *o, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

/* POST /api/time (spec §10.3): {"epoch": <UTC seconds>, "tz_iana": ..., "tz_posix": ...}, the
 * zone optional. The app task notices the jump and schedules again (main/app.c). */
static void set_time(const char *body, uint8_t *out, size_t size, webui_reply_t *reply)
{
    cJSON *in = cJSON_Parse(body);
    double epoch = 0;
    const cJSON *iana = cJSON_GetObjectItemCaseSensitive(in, "tz_iana");
    const cJSON *posix = cJSON_GetObjectItemCaseSensitive(in, "tz_posix");
    bool zone = cJSON_IsString(iana) && cJSON_IsString(posix);
    if (!json_number(in, "epoch", &epoch) || epoch < MIN_EPOCH || epoch > MAX_EPOCH) {
        reply_error(reply, out, size, 400, "epoch: UTC seconds between 2020 and 2099");
    } else if (zone && (strlen(iana->valuestring) >= SETTINGS_TZ_IANA_LEN || iana->valuestring[0] == '\0' ||
                        strlen(posix->valuestring) >= SETTINGS_TZ_POSIX_LEN || posix->valuestring[0] == '\0')) {
        reply_error(reply, out, size, 400, "tz_iana and tz_posix: a zone name and its POSIX rule");
    } else if (timekeeping_set_utc((time_t)epoch) != ESP_OK) {
        reply_error(reply, out, size, 500, "the RTC didn't take the time");
    } else {
        if (zone) {
            app_ui_set_zone(iana->valuestring, posix->valuestring);
        }
        ESP_LOGI(TAG, "time set from the web UI%s", zone ? ", with the zone" : "");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        reply_cjson(reply, out, size, o);
    }
    cJSON_Delete(in);
}

/* POST /api/battery/learn (D21): {"start": true} arms learning from the next full discharge,
 * {"stop": true} ends it. */
static void learn(const char *body, uint8_t *out, size_t size, webui_reply_t *reply)
{
    cJSON *in = cJSON_Parse(body);
    bool start = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "start"));
    bool stop = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "stop"));
    cJSON_Delete(in);
    if (start == stop) {
        reply_error(reply, out, size, 400, "send {\"start\": true} or {\"stop\": true}");
        return;
    }
    if (start && !timekeeping_valid()) {
        reply_error(reply, out, size, 409, "set the clock first");
        return;
    }
    app_ui_learn(start);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", battery_learn_state_name(battery_learn_state(sensors_learn())));
    reply_cjson(reply, out, size, o);
}

/* A backup of the largest files restores: settings.json up to the 4 KB app_ui.c keeps, presets.json up to
 * its own limit, and the bundle around them; and of the deepest, as each limit leaves room for the next. */
_Static_assert(SETTINGS_FILE_MAX + UI_PRESETS_JSON_MAX + 512 <= WEBUI_BODY_MAX,
               "a backup of the largest files fits a request");
_Static_assert(UI_JSON_MAX_DEPTH + 2 <= BACKUP_MAX_DEPTH, "a backup holds the deepest presets.json");
_Static_assert(BACKUP_MAX_DEPTH <= WEBUI_JSON_MAX_DEPTH, "the web server passes the deepest backup");

static const char *step_result_name(uint8_t r)
{
    static const char *const k_results[] = { "not run", "ok", "failed", "kept" };
    return k_results[r < 4 ? r : 0];
}

/* GET /api/pc (spec §10.3, M7): the session now, the PC's catalogue, the last PC step and Test; never the token. */
static void pc_state(uint8_t *out, size_t size, webui_reply_t *reply)
{
    app_pc_status_t s;
    app_pc_status(&s);
    const settings_t *set = app_settings();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", set->pc_enabled);
    cJSON_AddBoolToObject(o, "session", s.session);
    cJSON_AddBoolToObject(o, "connected", s.connected);
    cJSON_AddBoolToObject(o, "demo", s.demo);
    cJSON_AddBoolToObject(o, "set_always", s.set_always);
    cJSON_AddStringToObject(o, "name", s.name);
    cJSON_AddStringToObject(o, "hwinfo", pc_hwinfo_name(s.hwinfo));
    cJSON_AddStringToObject(o, "preset", s.preset);
    cJSON_AddNumberToObject(o, "since", (double)s.since);
    cJSON_AddNumberToObject(o, "age_ms", s.age_ms);
    cJSON_AddStringToObject(o, "trouble", s.why);
    const ui_pc_t *view = app_pc_ui();
    cJSON *list = cJSON_AddArrayToObject(o, "catalog");
    for (int i = 0; view->catalog != NULL && i < view->catalog->count; i++) {
        const pc_reading_t *r = &view->catalog->r[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", r->id);
        cJSON_AddStringToObject(e, "label", r->label);
        cJSON_AddStringToObject(e, "unit", r->unit);
        cJSON_AddItemToArray(list, e);
    }
    const app_sync_state_t *sync = &app_state()->sync;
    cJSON *step = cJSON_AddObjectToObject(o, "step");
    cJSON_AddNumberToObject(step, "at", sync->last_at);
    cJSON_AddStringToObject(step, "result", step_result_name(sync->last_result[SYNC_STEP_PC]));
    cJSON_AddStringToObject(step, "detail", sync->last_detail[SYNC_STEP_PC]);
    const app_pc_test_t *t = app_pc_last_test();
    cJSON *test = cJSON_AddObjectToObject(o, "test");
    cJSON_AddNumberToObject(test, "at", (double)t->at);
    cJSON_AddStringToObject(test, "result", step_result_name(t->result));
    cJSON_AddStringToObject(test, "detail", t->detail);
    cJSON_AddBoolToObject(o, "testing", app_sync_active() && sync_step() == SYNC_STEP_PC);
    reply_cjson(reply, out, size, o);
}

/* GET /api/backup (spec §14.4): the /cfg files as the firmware would save them now. */
static void backup(uint8_t *out, size_t size, webui_reply_t *reply)
{
    EXT_RAM_BSS_ATTR static char settings[SETTINGS_FILE_MAX];
    EXT_RAM_BSS_ATTR static char presets[UI_PRESETS_JSON_MAX];
    netmgr_status_t net;
    netmgr_status(&net);
    backup_file_t files[] = {
        { "settings.json", app_ui_saved_settings_json(settings, sizeof(settings)) ? settings : NULL },
        { "presets.json", app_ui_saved_presets_json(presets, sizeof(presets)) ? presets : NULL },
    };
    reply_text(reply, out, size,
               backup_build(files, 2, net.host, esp_app_get_description()->version, (char *)out, size));
}

/* GET /api/settings (spec §10.3): the file's settings, and which keys are set, never the keys. */
static void get_settings(uint8_t *out, size_t size, webui_reply_t *reply)
{
    EXT_RAM_BSS_ATTR static char text[SETTINGS_FILE_MAX];
    cJSON *o = app_ui_settings_json(text, sizeof(text)) > 0 ? cJSON_Parse(text) : NULL;
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        reply_error(reply, out, size, 500, "the settings don't print");
        return;
    }
    cJSON *keys = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(o, "solar"), "keys");
    cJSON_AddBoolToObject(keys, "fs_key", app_secret_set(SETTINGS_SECRET_FS_KEY));
    cJSON_AddBoolToObject(keys, "solcast_key", app_secret_set(SETTINGS_SECRET_SOLCAST_KEY));
    cJSON_AddNumberToObject(keys, "solcast_sites", app_secret_set(SETTINGS_SECRET_SOLCAST_SITE1) +
                                                       app_secret_set(SETTINGS_SECRET_SOLCAST_SITE2));
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(o, "pc"), "keys"), "token",
                          app_secret_set(SETTINGS_SECRET_PC_TOKEN)); /* M7 */
    keys = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(o, "energy"), "keys");
    cJSON_AddBoolToObject(keys, "solax_token", app_secret_set(SETTINGS_SECRET_SOLAX_TOKEN));
    cJSON_AddBoolToObject(keys, "solax_sn", app_secret_set(SETTINGS_SECRET_SOLAX_SN));
    cJSON_AddBoolToObject(keys, "solax_client_id", app_secret_set(SETTINGS_SECRET_SOLAX_CLIENT_ID));
    cJSON_AddBoolToObject(keys, "solax_client_secret", app_secret_set(SETTINGS_SECRET_SOLAX_CLIENT_SECRET));
    reply_cjson(reply, out, size, o);
}

/* PATCH /api/settings (spec §10.3, §14.3): the keys to NVS `secrets`, the rest merged into the file; a second
 * Forecast.Solar plane without a key is refused (spec §11.5). */
static void patch_settings(const char *body, uint8_t *out, size_t size, webui_reply_t *reply)
{
    EXT_RAM_BSS_ATTR static char clean[SETTINGS_FILE_MAX], base[SETTINGS_FILE_MAX], merged[SETTINGS_FILE_MAX];
    EXT_RAM_BSS_ATTR static settings_secrets_t secrets;
    EXT_RAM_BSS_ATTR static settings_t next;
    char err[112] = "the settings don't fit";
    bool ok = settings_take_secrets(body, clean, sizeof(clean), &secrets, err, sizeof(err)) > 0 &&
              app_ui_settings_json(base, sizeof(base)) > 0 &&
              settings_patch(base, clean, merged, sizeof(merged), err, sizeof(err)) > 0 &&
              settings_from_json(merged, app_settings(), &next, err, sizeof(err));
    bool fs_key = secrets.given[SETTINGS_SECRET_FS_KEY] ? secrets.value[SETTINGS_SECRET_FS_KEY][0] != '\0'
                                                         : app_secret_set(SETTINGS_SECRET_FS_KEY);
    ok = ok && settings_check_solar(&next, fs_key, err, sizeof(err)) &&
         app_ui_patch_settings(clean, err, sizeof(err)) == ESP_OK;
    esp_err_t saved = ok ? app_secrets_apply(&secrets) : ESP_OK;
    memset(&secrets, 0, sizeof(secrets)); /* the keys stay in NVS alone */
    if (!ok) {
        reply_error(reply, out, size, 400, err);
    } else if (saved != ESP_OK) {
        reply_error(reply, out, size, 500, "the keys weren't saved");
    } else {
        get_settings(out, size, reply);
    }
}

/* POST /api/restore: every file is checked before any is replaced. */
static void restore(const char *body, uint8_t *out, size_t size, webui_reply_t *reply)
{
    EXT_RAM_BSS_ATTR static char buf[WEBUI_BODY_MAX];
    EXT_RAM_BSS_ATTR static ui_presets_t presets;
    backup_file_t files[8];
    char err[UI_PRESET_ERR_LEN + 16]; /* a file's name before its reason */
    int n = backup_split(body, files, 8, buf, sizeof(buf), err, sizeof(err));
    if (n < 0) {
        reply_error(reply, out, size, 400, err);
        return;
    }
    const char *settings = NULL, *presets_text = NULL;
    cJSON *skipped = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        if (strcmp(files[i].name, "settings.json") == 0) {
            settings = files[i].text;
        } else if (strcmp(files[i].name, "presets.json") == 0) {
            presets_text = files[i].text;
        } else {
            cJSON_AddItemToArray(skipped, cJSON_CreateString(files[i].name)); /* from a later firmware */
        }
    }
    char why[UI_PRESET_ERR_LEN];
    EXT_RAM_BSS_ATTR static char clean[SETTINGS_FILE_MAX];
    EXT_RAM_BSS_ATTR static settings_secrets_t dropped;
    EXT_RAM_BSS_ATTR static settings_t next;
    bool settings_ok = true;
    if (settings != NULL) { /* a key a bundle carries never reaches the file (spec §14.4) */
        settings_ok = settings_take_secrets(settings, clean, sizeof(clean), &dropped, why, sizeof(why)) > 0 &&
                      settings_from_json(clean, app_settings(), &next, why, sizeof(why));
        memset(&dropped, 0, sizeof(dropped));
        settings = clean;
    }
    if (!settings_ok) {
        snprintf(err, sizeof(err), "settings.json: %s", why);
    } else if (presets_text != NULL && !ui_presets_from_json(presets_text, &presets, why, sizeof(why))) {
        snprintf(err, sizeof(err), "presets.json: %s", why);
    } else {
        err[0] = '\0';
    }
    if (err[0] != '\0') {
        cJSON_Delete(skipped);
        reply_error(reply, out, size, 400, err);
        return;
    }
    esp_err_t e = settings != NULL ? app_ui_replace_settings(settings, why, sizeof(why)) : ESP_OK;
    if (e == ESP_OK && presets_text != NULL) {
        e = app_ui_replace_presets(&presets);
    }
    if (e != ESP_OK) {
        cJSON_Delete(skipped);
        reply_error(reply, out, size, 500, "saving failed; some files may be restored");
        return;
    }
    ESP_LOGI(TAG, "restored %d file(s)", n - cJSON_GetArraySize(skipped));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddItemToObject(o, "skipped", skipped);
    cJSON *notes = cJSON_AddArrayToObject(o, "notes"); /* what waits: keys never come with a bundle (spec §14.4) */
    if (settings != NULL && !settings_check_solar(&next, app_secret_set(SETTINGS_SECRET_FS_KEY), why, sizeof(why))) {
        char note[sizeof(why) + 56]; /* the reason and the 52 characters after it */
        snprintf(note, sizeof(note), "%s: the forecast uses the first plane until one is set", why);
        cJSON_AddItemToArray(notes, cJSON_CreateString(note));
    }
    reply_cjson(reply, out, size, o);
}

void app_web_api(const char *method, const char *path, const char *query, const char *body, uint8_t *out,
                 size_t size, webui_reply_t *reply)
{
    EXT_RAM_BSS_ATTR static ui_presets_t presets;
    char err[UI_PRESET_ERR_LEN];
    bool get = strcmp(method, "GET") == 0;
    if (strcmp(path, "/api/status") == 0 && get) {
        get_status(out, size, reply);
    } else if (strcmp(path, "/api/settings") == 0 && get) {
        get_settings(out, size, reply);
    } else if (strcmp(path, "/api/settings") == 0 && strcmp(method, "PATCH") == 0) {
        patch_settings(body, out, size, reply);
    } else if (strcmp(path, "/api/layouts") == 0 && get) {
        reply_text(reply, out, size, ui_catalog_layouts_json((char *)out, size));
    } else if (strcmp(path, "/api/fields") == 0 && get) {
        ui_context_t ctx;
        app_ui_context(&ctx);
        reply_text(reply, out, size, ui_catalog_fields_json(&ctx, (char *)out, size));
    } else if (strcmp(path, "/api/presets") == 0 && get) {
        reply_text(reply, out, size, ui_presets_to_json(app_presets(), (char *)out, size));
    } else if (strcmp(path, "/api/presets") == 0 && strcmp(method, "PUT") == 0) {
        if (!ui_presets_from_json(body, &presets, err, sizeof(err))) {
            reply_error(reply, out, size, 400, err);
        } else if (app_ui_replace_presets(&presets) != ESP_OK) {
            reply_error(reply, out, size, 500, "presets.json wasn't saved");
        } else {
            reply_text(reply, out, size, ui_presets_to_json(app_presets(), (char *)out, size));
        }
    } else if (strcmp(path, "/api/preview.bmp") == 0 && (get || strcmp(method, "POST") == 0)) {
        preview(method, query, body, out, size, reply);
    } else if (strcmp(path, "/api/screenshot.bmp") == 0 && get) {
        reply_bmp(display_fb(), out, size, reply);
    } else if (strcmp(path, "/api/time") == 0 && strcmp(method, "POST") == 0) {
        set_time(body, out, size, reply);
    } else if (strcmp(path, "/api/battery/learn") == 0 && strcmp(method, "POST") == 0) {
        learn(body, out, size, reply);
    } else if (strcmp(path, "/api/sync") == 0 && strcmp(method, "POST") == 0) { /* spec §10.3, M5 */
        esp_err_t e = app_sync_now();
        if (e == ESP_OK) {
            reply_text(reply, out, size, (size_t)snprintf((char *)out, size, "{\"started\":true}"));
            reply->status = 202; /* the page follows it in GET /api/status */
        } else {
            reply_error(reply, out, size, 409,
                        e == ESP_ERR_NOT_FOUND       ? "no Wi-Fi network is saved"
                        : e == ESP_ERR_INVALID_STATE ? "not now: a sync runs, the battery is critical, or the device "
                                                       "is on its own network only"
                                                     : "the sync didn't start");
        }
    } else if (strcmp(path, "/api/solar/check") == 0 && strcmp(method, "POST") == 0) { /* spec §10.3, M6d */
        esp_err_t e = app_sync_check();
        if (e == ESP_OK) {
            reply_text(reply, out, size, (size_t)snprintf((char *)out, size, "{\"started\":true}"));
            reply->status = 202; /* the page follows it in GET /api/status */
        } else {
            reply_error(reply, out, size, 409,
                        e == ESP_ERR_NOT_FOUND       ? "no Wi-Fi network is saved"
                        : e == ESP_ERR_INVALID_ARG   ? "nothing to check: the forecast and the house's energy are off"
                        : e == ESP_ERR_INVALID_STATE ? "not now: a sync runs, the battery is critical, or the device "
                                                       "is on its own network only"
                                                     : "the check didn't start");
        }
    } else if (strcmp(path, "/api/pc") == 0 && get) { /* spec §10.3, M7 */
        pc_state(out, size, reply);
    } else if (strcmp(path, "/api/pc/test") == 0 && strcmp(method, "POST") == 0) {
        esp_err_t e = app_sync_pc_test();
        if (e == ESP_OK) {
            reply_text(reply, out, size, (size_t)snprintf((char *)out, size, "{\"started\":true}"));
            reply->status = 202; /* the page follows it in GET /api/pc */
        } else {
            reply_error(reply, out, size, 409,
                        e == ESP_ERR_NOT_FOUND       ? "no Wi-Fi network is saved"
                        : e == ESP_ERR_INVALID_ARG   ? "nothing to test: PC stats are off or the PC has no address"
                        : e == ESP_ERR_INVALID_STATE ? "not now: a sync runs, the battery is critical, or the device "
                                                       "is on its own network only"
                                                     : "the test didn't start");
        }
    } else if (strcmp(path, "/api/backup") == 0 && get) {
        backup(out, size, reply);
    } else if (strcmp(path, "/api/restore") == 0 && strcmp(method, "POST") == 0) {
        restore(body, out, size, reply);
    } else if (strcmp(path, "/api/lora/reset") == 0 && strcmp(method, "POST") == 0) {
        lora_widget_reset_stats();
        reply_text(reply, out, size, (size_t)snprintf((char *)out, size, "{\"ok\":true}"));
    } else if (strcmp(path, "/api/lora/stats") == 0 && get) {
        lora_stats_t ls = lora_widget_get_stats();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", ls.enabled);
        cJSON_AddBoolToObject(o, "initialized", ls.initialized);
        cJSON_AddNumberToObject(o, "ble_pin", ls.ble_pin);
        cJSON_AddBoolToObject(o, "ble_connected", ls.ble_connected);
        cJSON_AddNumberToObject(o, "packet_count", ls.packet_count);
        cJSON_AddNumberToObject(o, "packets_sent", ls.packets_sent);
        cJSON_AddNumberToObject(o, "recv_flood", ls.recv_flood);
        cJSON_AddNumberToObject(o, "recv_direct", ls.recv_direct);
        cJSON_AddNumberToObject(o, "noise_floor_dbm", ls.noise_floor_dbm);
        cJSON_AddNumberToObject(o, "last_rssi_dbm", ls.last_rssi_dbm);
        cJSON_AddNumberToObject(o, "last_snr_db", (double)ls.last_snr_q4 / 4.0);
        ds_entry_t eb;
        if (ds_get(app_ds(), DS_BAT_LEVEL, &eb) && eb.updated != 0) {
            cJSON_AddNumberToObject(o, "battery_mv", eb.mv);
            cJSON_AddNumberToObject(o, "battery_level", eb.value);
        }
        reply_cjson(reply, out, size, o);
    } else if (strcmp(path, "/api/lora/companion") == 0 && strcmp(method, "POST") == 0) {
        uint8_t cmd_buf[COMPANION_MAX_FRAME_SIZE];
        size_t cmd_len = 0;
        if (body != NULL && body[0] == '{') {
            cJSON *in = cJSON_Parse(body);
            const cJSON *h = in ? cJSON_GetObjectItemCaseSensitive(in, "hex") : NULL;
            const cJSON *cmd_id = in ? cJSON_GetObjectItemCaseSensitive(in, "cmd") : NULL;
            if (cJSON_IsString(h) && h->valuestring) {
                size_t hlen = strlen(h->valuestring);
                for (size_t i = 0; i < hlen && cmd_len < sizeof(cmd_buf); i += 2) {
                    unsigned int byte = 0;
                    if (sscanf(h->valuestring + i, "%02x", &byte) == 1) {
                        cmd_buf[cmd_len++] = (uint8_t)byte;
                    }
                }
            } else if (cJSON_IsNumber(cmd_id)) {
                cmd_buf[cmd_len++] = (uint8_t)cmd_id->valueint;
            }
            if (in) cJSON_Delete(in);
        } else if (body != NULL) {
            cmd_len = strlen(body);
            if (cmd_len > sizeof(cmd_buf)) cmd_len = sizeof(cmd_buf);
            memcpy(cmd_buf, body, cmd_len);
        }
        if (cmd_len == 0) {
            reply_error(reply, out, size, 400, "missing companion command");
        } else {
            app_ui_state_t *s = app_state();
            bool changed = false;
            uint8_t resp[COMPANION_MAX_FRAME_SIZE];
            size_t resp_len = lora_widget_handle_companion_cmd(cmd_buf, cmd_len, &s->settings.lora, &changed, resp, sizeof(resp));
            if (changed) {
                app_lora_apply();
                app_ui_save_settings();
            }
            cJSON *res = cJSON_CreateObject();
            cJSON_AddBoolToObject(res, "ok", true);
            cJSON_AddNumberToObject(res, "resp_len", (double)resp_len);
            char hex_out[COMPANION_MAX_FRAME_SIZE * 2 + 1];
            for (size_t i = 0; i < resp_len; i++) {
                snprintf(hex_out + (i * 2), 3, "%02x", resp[i]);
            }
            hex_out[resp_len * 2] = '\0';
            cJSON_AddStringToObject(res, "hex", hex_out);
            if (resp_len >= 11 && resp[0] == RESP_CODE_BATT_AND_STORAGE) {
                uint16_t mv = (uint16_t)resp[1] | ((uint16_t)resp[2] << 8);
                uint32_t used_kb = (uint32_t)resp[3] | ((uint32_t)resp[4] << 8) | ((uint32_t)resp[5] << 16) | ((uint32_t)resp[6] << 24);
                uint32_t total_kb = (uint32_t)resp[7] | ((uint32_t)resp[8] << 8) | ((uint32_t)resp[9] << 16) | ((uint32_t)resp[10] << 24);
                cJSON_AddNumberToObject(res, "batt_mv", mv);
                cJSON_AddNumberToObject(res, "storage_used_kb", (double)used_kb);
                cJSON_AddNumberToObject(res, "storage_total_kb", (double)total_kb);
            }
            reply_cjson(reply, out, size, res);
        }
    } else if (strcmp(path, "/api/lora/advert") == 0 && strcmp(method, "POST") == 0) {
        bool flood = true;
        if (body != NULL && body[0] != '\0') {
            cJSON *in = cJSON_Parse(body);
            if (in != NULL) {
                const cJSON *f = cJSON_GetObjectItemCaseSensitive(in, "flood");
                if (cJSON_IsBool(f)) {
                    flood = cJSON_IsTrue(f);
                }
                cJSON_Delete(in);
            }
        }
        esp_err_t err = lora_widget_send_advert(flood);
        if (err == ESP_OK) {
            reply_text(reply, out, size, (size_t)snprintf((char *)out, size, "{\"ok\":true}"));
        } else {
            reply_error(reply, out, size, 500, "failed to send advert");
        }
    } else if (strcmp(path, "/api/lora/discover") == 0 && strcmp(method, "POST") == 0) {
        esp_err_t err = lora_widget_send_discover();
        if (err == ESP_OK) {
            reply_text(reply, out, size, (size_t)snprintf((char *)out, size, "{\"ok\":true}"));
        } else {
            reply_error(reply, out, size, 500, "failed to send discover");
        }
    } else if (strcmp(path, "/api/lora/send") == 0 && strcmp(method, "POST") == 0) {
        cJSON *in = cJSON_Parse(body);
        const cJSON *t = in != NULL ? cJSON_GetObjectItemCaseSensitive(in, "text") : NULL;
        if (!cJSON_IsString(t) || t->valuestring == NULL || t->valuestring[0] == '\0') {
            cJSON_Delete(in);
            reply_error(reply, out, size, 400, "missing text");
        } else {
            esp_err_t err = lora_widget_send_channel_msg(t->valuestring);
            cJSON_Delete(in);
            if (err == ESP_OK) {
                reply_text(reply, out, size, (size_t)snprintf((char *)out, size, "{\"ok\":true}"));
            } else {
                reply_error(reply, out, size, 500, "failed to send message");
            }
        }
    } else if (strcmp(path, "/api/lora/cli") == 0 && strcmp(method, "POST") == 0) {
        cJSON *in = cJSON_Parse(body);
        const cJSON *cmd = in != NULL ? cJSON_GetObjectItemCaseSensitive(in, "cmd") : NULL;
        if (!cJSON_IsString(cmd) || cmd->valuestring == NULL) {
            cJSON_Delete(in);
            reply_error(reply, out, size, 400, "missing cmd");
        } else {
            app_ui_state_t *s = app_state();
            bool changed = false;
            char reply_buf[512] = { 0 };
            lora_widget_exec_cli(cmd->valuestring, &s->settings.lora, &changed, reply_buf, sizeof(reply_buf));
            cJSON_Delete(in);
            if (changed) {
                app_lora_apply();
                app_ui_save_settings();
            }
            cJSON *res = cJSON_CreateObject();
            cJSON_AddBoolToObject(res, "ok", true);
            cJSON_AddStringToObject(res, "reply", reply_buf);
            reply_cjson(reply, out, size, res);
        }
    } else {
        reply_error(reply, out, size, 404, "no such API");
    }
}
