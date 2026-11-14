#include "app.h"

#include <sys/time.h>
#include <time.h>

#include "app_internal.h"

#include "board.h"
#include "board_buttons.h"
#include "board_pins.h"
#include "cJSON.h"
#include "diag.h"
#include "lang.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power.h"
#include "pcf85063.h"
#include "scheduler.h"
#include "st7305.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "lora_widget.h"
#include "netmgr.h"
#include "storage.h"
#include "timekeeping.h"
#include "ui_screens.h"
#include "util_snapshot.h"
#include "util_ticks.h"

#define APP_STACK         12288 /* internal RAM: deep-sleep entry requires it */
#define APP_PRIORITY      5
#define APP_CORE          1
#define QUEUE_DEPTH       16
#define GRACE_MS          2000 /* stay awake after boot or a button so a PC can find the board */
#define TETHER_RECHECK_MS 1000
#define RETRY_S           300  /* after a failed boot with no PC attached */
#define SNAP_MAGIC        0x72666c62u /* "rflb" */
#define SNAP_VERSION      20 /* 6: the weather, the air quality and the syncs' state; 7: the rain; 8: split presets;
                                   9: 24 cells (M6c); 10: the solar state and the sync's two steps (M6d);
                                   11: the Developer API's plant (D37); 12: the presets' cycle windows (D41);
                                   13: the presets' titles (D42); 14: the last good sync and the presets'
                                   status_sync (D43); 15: the forecast's setup; 16: the presets apart (M7);
                                   17: the PC step (M7); 18: lora config; 19: MeshCore lora config */
#define PRESETS_MAGIC     0x7266707au /* "rfpz": the presets' block */
#define PRESETS_VERSION   1
#define PEEK_MS           60000 /* a button during the night shows the dashboard this long (spec §9.1) */
#define NIGHT_RECHECK_S   60    /* a night sleep with a button held looks again this often (D16) */
#define CRITICAL_RECHECK_S 600  /* the critical sleep checks again this often if KEY is held */
#define OTA_VERIFY_MS     60000 /* spec §10.5: a new image is valid after this long without a panic */
#define MENU_SETTLE_MS    1000  /* BOOT pressed fast to back out of the menu makes no double after it */
#define RESTART_MS        1500  /* a restart's toast stays this long; it also lets a web reply go out */
#define NVS_KEY_RESUME    "resume_cfg" /* in `sys`: come back in config mode (spec §10.2) */

#if CONFIG_REFLBO_PANEL_INIT_XIAOZHI
#define PANEL_VARIANT ST7305_VARIANT_XIAOZHI
#else
#define PANEL_VARIANT ST7305_VARIANT_FACTORY
#endif

static const char *TAG = "app";

typedef enum {
    EV_RTC_ALARM,
    EV_BUTTON,
    EV_CALL,
} ev_type_t;

typedef struct {
    ev_type_t type;
    union {
        struct {
            board_button_t button;
            gesture_t gesture;
        } button;
        struct {
            void (*fn)(void *);
            void *arg;
            SemaphoreHandle_t done;
        } call;
    };
} app_event_t;

/* Kept in RTC RAM through deep sleep; invalid after any other reset (spec §3.3). */
typedef struct {
    util_snapshot_hdr_t hdr;
    sensors_state_t sensors;
    display_state_t display;
    app_ui_state_t ui;
    time_t next_alarm;
} app_snapshot_t;
_Static_assert(sizeof(app_snapshot_t) <= 7168, "the RTC-RAM snapshot is at most 7 KB (spec §6, M6c, D42)");

static RTC_NOINIT_ATTR app_snapshot_t s_snap;

/* The presets, with their titles and from M7 the PC ids and the visuals, in RTC FAST memory, which deep sleep keeps
 * powered as it holds heap too (CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP): RTC slow memory had 330 bytes
 * left (spec §6). Its own seal, so either block found broken makes the wake cold. */
typedef struct {
    util_snapshot_hdr_t hdr;
    ui_presets_t presets;
} app_presets_snapshot_t;
_Static_assert(sizeof(app_presets_snapshot_t) <= 4608,
               "the presets' block leaves RTC FAST memory 3.5 KB of heap at least (spec §6, M7)");
static RTC_FAST_ATTR app_presets_snapshot_t s_presets_snap;
static QueueHandle_t s_queue;
static time_t s_next_alarm; /* the RTC alarm: the next minute slot */
static time_t s_wake_at;    /* the earliest wake: the alarm, or a cycle switch or seconds tick before it */
static int64_t s_peek_until_ms; /* night: the dashboard shows until then (app_uptime_ms) */
static bool s_ota_pending;      /* this image came from an upload and is not yet marked valid */
static bool s_rendered;         /* the first frame went out since boot */

static void IRAM_ATTR __attribute__((unused)) on_rtc_int(void *arg)
{
    (void)arg;
    app_event_t ev = { .type = EV_RTC_ALARM };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_queue, &ev, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void on_button(board_button_t button, gesture_t gesture)
{
    app_event_t ev = { .type = EV_BUTTON, .button = { .button = button, .gesture = gesture } };
    xQueueSend(s_queue, &ev, pdMS_TO_TICKS(100));
}

esp_err_t app_execute(void (*fn)(void *arg), void *arg)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(done != NULL, ESP_ERR_NO_MEM, TAG, "semaphore");
    app_event_t ev = { .type = EV_CALL, .call = { .fn = fn, .arg = arg, .done = done } };
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(done);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return ESP_OK;
}

esp_err_t app_post(void (*fn)(void *arg), void *arg)
{
    app_event_t ev = { .type = EV_CALL, .call = { .fn = fn, .arg = arg, .done = NULL } };
    return xQueueSend(s_queue, &ev, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void on_lora_update_post(void *arg)
{
    (void)arg;
    app_ui_tick(false);
}

static void on_lora_notify(void)
{
    app_post(on_lora_update_post, NULL);
}

/* A restart the web UI asked for (an update, Restart) comes back in config mode, so the page finds
 * the device again (spec §10.2). The flag is in NVS, not RTC RAM: another image lays RTC RAM out
 * differently, and an update or a rollback is just such a restart. */
static void resume_config_set(bool on)
{
    nvs_handle_t nvs;
    if (nvs_open("sys", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    esp_err_t err = on ? nvs_set_u8(nvs, NVS_KEY_RESUME, 1) : nvs_erase_key(nvs, NVS_KEY_RESUME);
    if (err == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static bool resume_config_get(void)
{
    nvs_handle_t nvs;
    uint8_t on = 0;
    if (nvs_open("sys", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_RESUME, &on);
        nvs_close(nvs);
    }
    return on != 0;
}

void app_restart(lang_str_t message, bool config_after)
{
    resume_config_set(config_after);
    app_menu_close();
    app_ui_toast(lang_str(lang_get(app_settings()->language), message));
    vTaskDelay(pdMS_TO_TICKS(RESTART_MS));
    esp_restart();
}

void app_factory_reset(void)
{
    app_menu_close();
    app_ui_toast(lang_str(lang_get(app_settings()->language), LS_T_RESETTING));
    esp_err_t err = storage_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erasing storage: %s", esp_err_to_name(err));
    }
    static const char *const k_namespaces[] = { "wifi", "secrets", "ctr" };
    for (size_t i = 0; i < sizeof(k_namespaces) / sizeof(k_namespaces[0]); i++) {
        nvs_handle_t nvs;
        if (nvs_open(k_namespaces[i], NVS_READONLY, &nvs) != ESP_OK) {
            continue; /* never written: nothing to erase */
        }
        nvs_close(nvs);
        if (nvs_open(k_namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    ESP_LOGW(TAG, "factory reset done; restarting");
    vTaskDelay(pdMS_TO_TICKS(RESTART_MS));
    esp_restart();
}

static void schedule_next_at(time_t now)
{
    sched_wake_t wake = app_ui_next_wake(now);
    s_wake_at = wake.when;
    if (wake.alarm != s_next_alarm) {
        s_next_alarm = wake.alarm;
        esp_err_t err = pcf85063_set_alarm(s_next_alarm); /* also clears the alarm flag, which releases INT */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RTC alarm: %s; the backup timer takes over", esp_err_to_name(err));
        }
    }
}

/* The timer wake: a cycle switch or seconds tick if one comes before the alarm, else the alarm itself. */
static time_t sleep_until(void)
{
    if (s_wake_at == 0) return s_next_alarm;
    if (s_next_alarm == 0) return s_wake_at;
    return s_wake_at < s_next_alarm ? s_wake_at : s_next_alarm;
}

int64_t app_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

bool app_focus_mode_active(void)
{
    ui_presets_t *p = app_presets();
    if (p == NULL || p->active >= p->count) {
        return false;
    }
    return ui_preset_is_focus(&p->presets[p->active]);
}

void app_lora_apply(void)
{
    app_ui_state_t *st = app_state();
    char psk[SETTINGS_SECRET_LEN];
    bool nvs_ready = false;
    if (app_secret_read(SETTINGS_SECRET_LORA_PSK, psk, sizeof(psk), &nvs_ready)) {
        snprintf(st->settings.lora.channel_psk, sizeof(st->settings.lora.channel_psk),
                 "%.*s", (int)(sizeof(st->settings.lora.channel_psk) - 1), psk);
    } else if (nvs_ready) {
        snprintf(st->settings.lora.channel_psk, sizeof(st->settings.lora.channel_psk), "%s", LORA_DEFAULT_PUBLIC_PSK);
    }
    memset(psk, 0, sizeof(psk));
    if (app_focus_mode_active()) {
        lora_widget_sync_config(&st->settings.lora);
        lora_widget_sleep();
        return;
    }
    lora_widget_apply(&st->settings.lora);
}

static void prepare_lora_work_cfg(lora_config_t *work, char *prev_psk, size_t prev_sz)
{
    *work = app_state()->settings.lora;
    char psk[SETTINGS_SECRET_LEN] = { 0 };
    bool nvs_ready = false;
    if (app_secret_read(SETTINGS_SECRET_LORA_PSK, psk, sizeof(psk), &nvs_ready)) {
        snprintf(work->channel_psk, sizeof(work->channel_psk), "%.*s",
                 (int)(sizeof(work->channel_psk) - 1), psk);
    } else if (work->channel_psk[0] == '\0' || nvs_ready) {
        snprintf(work->channel_psk, sizeof(work->channel_psk), "%s", LORA_DEFAULT_PUBLIC_PSK);
    }
    memset(psk, 0, sizeof(psk));
    snprintf(prev_psk, prev_sz, "%s", work->channel_psk);
}

static void commit_lora_work_cfg(lora_config_t *work, const char *prev_psk)
{
    bool psk_changed = strcmp(prev_psk, work->channel_psk) != 0;
    char new_psk[LORA_CHANNEL_PSK_LEN] = { 0 };
    snprintf(new_psk, sizeof(new_psk), "%s", work->channel_psk);
    app_state()->settings.lora = *work;
    if (psk_changed) {
        app_lora_sync_psk_secret(new_psk);
    }
    memset(new_psk, 0, sizeof(new_psk));
    app_lora_apply();
    app_ui_save_settings();
}

void app_lora_exec_cli(const char *cmd, char *reply, size_t reply_sz)
{
    lora_config_t work;
    char prev_psk[LORA_CHANNEL_PSK_LEN];
    prepare_lora_work_cfg(&work, prev_psk, sizeof(prev_psk));
    bool changed = false;
    lora_widget_exec_cli(cmd, &work, &changed, reply, reply_sz);
    if (changed) {
        commit_lora_work_cfg(&work, prev_psk);
    }
    memset(&work, 0, sizeof(work));
    memset(prev_psk, 0, sizeof(prev_psk));
}

size_t app_lora_handle_companion(const uint8_t *cmd, size_t cmd_len, uint8_t *resp, size_t resp_max)
{
    lora_config_t work;
    char prev_psk[LORA_CHANNEL_PSK_LEN];
    prepare_lora_work_cfg(&work, prev_psk, sizeof(prev_psk));
    bool changed = false;
    size_t resp_len = lora_widget_handle_companion_cmd(cmd, cmd_len, &work, &changed, resp, resp_max);
    if (changed) {
        commit_lora_work_cfg(&work, prev_psk);
    }
    memset(&work, 0, sizeof(work));
    memset(prev_psk, 0, sizeof(prev_psk));
    return resp_len;
}

static bool lora_rekey_cb(void)
{
    return app_lora_rekey() == ESP_OK;
}

void app_focus_mode_sync(bool entering)
{
    static bool s_in_focus = false;
    if (entering == s_in_focus) {
        return;
    }
    s_in_focus = entering;
    if (entering) {
        ESP_LOGI(TAG, "Entering Super Power Efficient mode (Focus clock)");
        lora_widget_sleep();
        netmgr_stop();
        /* Maintain 160 MHz CPU frequency during sleep cycles to prevent Octal PSRAM bus divider instability */
        app_state()->cycle_at = 0;
    } else {
        ESP_LOGI(TAG, "Exiting Super Power Efficient mode (Focus clock)");
        if (app_state()->settings.lora.enabled) {
            app_lora_apply();
        }
        if (app_state()->settings.sync_mode == SETTINGS_SYNC_ALWAYS) {
            app_sync_schedule();
        }
        if (app_presets()->cycle_enabled) {
            app_state()->cycle_at = time(NULL) + app_presets()->cycle_interval_s;
        }
    }
}

/* A scheduled wake: the RTC alarm, its backup, a cycle switch or a seconds tick. `force` also
 * samples and renders; `rtc_edge`: the RTC's minute alarm, as its second began. */
static void on_tick(bool force, bool rtc_edge)
{
    esp_err_t err = timekeeping_load_from_rtc(rtc_edge);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC read: %s", esp_err_to_name(err));
    }
    timekeeping_drift_correct();
    time_t now = time(NULL);
    if (now >= s_next_alarm) {
        s_next_alarm = 0; /* fired: program the next one, even if it is the same minute again */
    }
    if (app_ui_night() && now >= app_state()->night_until) { /* spec §9.1: the dashboard is back */
        app_ui_end_night();
        err = display_wake();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel wake: %s", esp_err_to_name(err));
        }
        force = true;
    }
    schedule_next_at(app_ui_tick(force));
}

void app_clock_moved(int64_t delta_s)
{
    sensors_shift_time(delta_s); /* the battery history keeps its spacing on the new clock */
    app_state()->sched_checked = time(NULL); /* entries the jump skipped don't run late */
    app_state()->cycle_at = 0; /* the next tick starts the cycle interval again, rather than switching at once */
    app_sync_schedule(); /* the next sync by the new clock */
    on_tick(true, false); /* show the new time now, not at the next slot */
}

static const char *preset_name_at(int index)
{
    const ui_presets_t *p = app_presets();
    if (index < 0 || index >= p->count) {
        return "";
    }
    return p->presets[index].name;
}

/* Dashboard bindings (spec §5.6); the menu has its own while it is open. */
static void handle_button(board_button_t button, gesture_t gesture)
{
    bool in_menu = app_menu_is_open();
    time_t btn_now = time(NULL);
    power_hold_awake_ms(GRACE_MS);
    if (app_ui_night()) { /* any press keeps the night's dashboard up, awake, as the peek lives in RAM */
        s_peek_until_ms = app_uptime_ms() + PEEK_MS;
        power_hold_awake_ms(PEEK_MS);
    }
    const lang_t *lang = lang_get(app_settings()->language);
    char text[64];
    if (button != BOARD_BUTTON_BOOT || gesture != GESTURE_SHORT) {
        app_radar_loop_stop(); /* spec §11.2: KEY short still switches the preset */
    }
    if (app_menu_is_open()) {
        bool held = gesture == GESTURE_LONG;
        app_menu_key(button == BOARD_BUTTON_KEY ? (held ? UI_MENU_KEY_SELECT : UI_MENU_KEY_NEXT)
                                                : (held ? UI_MENU_KEY_EXIT : UI_MENU_KEY_BACK));
    } else if (app_config_active()) { /* spec §5.6 */
        if (button == BOARD_BUTTON_KEY && gesture == GESTURE_SHORT) {
            app_config_key();
        } else if (button == BOARD_BUTTON_BOOT && gesture == GESTURE_LONG) {
            app_config_exit();
        }
    } else if (app_state()->critical) {
        app_ui_sample(time(NULL)); /* only a recovered battery leaves this screen (spec §8) */
        app_ui_render();
    } else if (app_ui_first_run()) { /* spec §5.5: KEY leads on to the dashboard or the menu */
        if (button == BOARD_BUTTON_BOOT && gesture == GESTURE_LONG) {
            app_config_enter();
        } else if (button == BOARD_BUTTON_KEY && (gesture == GESTURE_SHORT || gesture == GESTURE_LONG)) {
            app_ui_end_first_run();
            if (gesture == GESTURE_LONG) {
                app_menu_open();
            }
        }
    } else if (button == BOARD_BUTTON_KEY && gesture == GESTURE_SHORT) {
        int next = ui_presets_next_with_lora(app_presets(), app_state()->settings.sync_mode == SETTINGS_SYNC_ALWAYS,
                                             app_state()->settings.lora.enabled);
        snprintf(text, sizeof(text), "%s: %s", lang_str(lang, LS_T_PRESET), preset_name_at(next));
        app_ui_toast_arm(text);
        app_ui_select(next, true);
    } else if (button == BOARD_BUTTON_KEY && gesture == GESTURE_LONG) {
        app_menu_open();
    } else if (button == BOARD_BUTTON_BOOT && gesture == GESTURE_SHORT &&
               app_presets()->presets[app_presets()->active].layout == UI_LAYOUT_RADAR) {
        if (app_radar_loop_start()) {
            ESP_LOGI(TAG, "BOOT short: the radar's loop"); /* D28 */
        } else {
            ESP_LOGI(TAG, "BOOT short: a sync for a fresh frame"); /* D30: no hour to play */
            app_sync_now_toast();
        }
    } else if (button == BOARD_BUTTON_BOOT && gesture == GESTURE_SHORT &&
               app_presets()->presets[app_presets()->active].layout == UI_LAYOUT_LORA &&
               app_state()->settings.lora.enabled) {
        lora_widget_send_advert(true);
        app_ui_toast(lang_str(lang, LS_T_LORA_ADVERT_SENT));
    } else if (button == BOARD_BUTTON_BOOT && gesture == GESTURE_SHORT) {
        app_ui_sample(time(NULL));
        app_ui_render();
        ESP_LOGI(TAG, "BOOT short: sensors refreshed");
    } else if (button == BOARD_BUTTON_BOOT && gesture == GESTURE_DOUBLE) { /* spec §5.6, D31 */
        if (app_menu_closed_within(MENU_SETTLE_MS)) {
            ESP_LOGI(TAG, "BOOT double just after the menu closed: ignored"); /* the presses that backed out */
        } else {
            app_sync_toggle_always();
        }
    } else if (button == BOARD_BUTTON_BOOT && gesture == GESTURE_LONG) {
        app_config_enter(); /* 3 s on the dashboard (spec §5.6) */
    }
    schedule_next_at(in_menu ? time(NULL) : btn_now);
}

/* A button held since before the sleep (D16) makes no gesture until it has been released. */
static void ignore_held_buttons(void)
{
    unsigned masked = power_masked_buttons();
    if (masked & POWER_BUTTON_KEY) {
        board_buttons_ignore_until_released(BOARD_BUTTON_KEY);
    }
    if (masked & POWER_BUTTON_BOOT) {
        board_buttons_ignore_until_released(BOARD_BUTTON_BOOT);
    }
}

/* A button woke the chip in the night: the panel wakes and shows the dashboard for a while; the
 * press itself does nothing else (spec §9.1). */
static void night_peek(board_button_t button)
{
    board_buttons_ignore_until_released(button);
    esp_err_t err = display_wake();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel wake: %s", esp_err_to_name(err));
    }
    s_peek_until_ms = app_uptime_ms() + PEEK_MS;
    power_hold_awake_ms(PEEK_MS);
    on_tick(true, false);
}

/* If the clock moved back (`rtc set`, later SNTP), the pending alarm is further off than the next
 * slot from now: schedule again. Comparing with a fresh schedule stays right across DST changes
 * and for any update interval. A clock that moved forward is caught by the backup tick. */
static void check_clock_jump(void)
{
    if (s_next_alarm != 0 && app_ui_next_wake(time(NULL)).alarm < s_next_alarm) {
        ESP_LOGW(TAG, "clock moved back; scheduling again");
        on_tick(true, false);
    }
}

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void handle_event(const app_event_t *ev)
{
    switch (ev->type) {
    case EV_RTC_ALARM:
        on_tick(false, true);
        break;
    case EV_BUTTON:
        handle_button(ev->button.button, ev->button.gesture);
        break;
    case EV_CALL: {
        bool was_valid = timekeeping_valid();
        int64_t before = now_ms(), mono_before = app_uptime_ms();
        ev->call.fn(ev->call.arg);
        if (ev->call.done != NULL) {
            xSemaphoreGive(ev->call.done);
        }
        /* how far the wall clock jumped, beside the time the call took: a long command (`panel fps 5`,
         * a sync's report) is no clock move */
        int64_t moved = (now_ms() - before) - (app_uptime_ms() - mono_before);
        if (timekeeping_valid() != was_valid || moved < -1000 || moved > 1000) {
            app_clock_moved(moved / 1000); /* `rtc set`, a sync, and friends */
        }
        power_hold_awake_ms(GRACE_MS);
        break;
    }
    }
}

static void handle_wake(power_wake_t wake)
{
    timekeeping_drift_correct();
    switch (wake) {
    case POWER_WAKE_RTC:
        on_tick(false, true);
        break;
    case POWER_WAKE_TIMER: /* a cycle switch or seconds tick, or scheduled alarm */
        on_tick(false, s_next_alarm != 0 && time(NULL) >= s_next_alarm);
        break;
    case POWER_WAKE_KEY:
    case POWER_WAKE_BOOT:
        if (display_asleep()) { /* a night that had to sleep light: the press only wakes the panel */
            night_peek(wake == POWER_WAKE_KEY ? BOARD_BUTTON_KEY : BOARD_BUTTON_BOOT);
            break;
        }
        board_buttons_resync(); /* edges during sleep raised no interrupt */
        power_hold_awake_ms(GRACE_MS);
        time_t wake_now = time(NULL);
        if (wake == POWER_WAKE_KEY &&
            (board_buttons_pressed(BOARD_BUTTON_KEY) || board_buttons_busy() || uxQueueMessagesWaiting(s_queue) > 0) &&
            !app_ui_night() && !app_state()->critical && !app_config_shows_setup() && !app_menu_is_open()) {
            schedule_next_at(wake_now);
            break;
        }
        if (!app_focus_mode_active() && app_state() && app_state()->settings.lora.enabled) {
            lora_widget_resume_ble();
        }
        app_ui_sample(wake_now);
        app_ui_render();
        schedule_next_at(wake_now);
        break;
    default:
        if ((s_next_alarm != 0 && time(NULL) >= s_next_alarm) || (s_wake_at != 0 && time(NULL) >= s_wake_at)) {
            on_tick(false, s_next_alarm != 0 && time(NULL) >= s_next_alarm);
        }
        break;
    }
    ignore_held_buttons();
}

static void seal_snapshot(void)
{
    sensors_export(&s_snap.sensors);
    display_export(&s_snap.display);
    app_ui_export(&s_snap.ui);
    app_ui_export_presets(&s_presets_snap.presets);
    s_snap.next_alarm = s_next_alarm;
    util_snapshot_seal(&s_snap, sizeof(s_snap), SNAP_MAGIC, SNAP_VERSION);
    util_snapshot_seal(&s_presets_snap, sizeof(s_presets_snap), PRESETS_MAGIC, PRESETS_VERSION);
}

void app_seal_snapshot(void)
{
    seal_snapshot();
}

/* Seals the RTC-RAM snapshot and holds the panel pins; false if the holds failed, in which case
 * the chip must not deep-sleep (the panel would reset). */
static bool prepare_deep_sleep(void)
{
    seal_snapshot();
    esp_err_t err = display_prepare_deep_sleep();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel pins: %s; light sleep this time", esp_err_to_name(err));
        display_cancel_deep_sleep();
        s_snap.hdr.magic = 0;
        s_presets_snap.hdr.magic = 0;
        return false;
    }
    lora_widget_sleep();
    return true;
}

/* Deep sleep until `wake` (the timer) or the RTC alarm or a button. */
static void enter_deep_sleep(time_t wake)
{
    if (!prepare_deep_sleep()) {
        handle_wake(power_sleep_light(wake));
        return;
    }
    power_sleep_deep(wake);
}

/* Night sleep (spec §9.1): the panel sleeps, and the chip deep-sleeps whatever the idle strategy
 * and even while tethered, until the end time or a button. */
static void enter_night_sleep(void)
{
    time_t until = app_state()->night_until;
    esp_err_t err = display_sleep();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel sleep: %s", esp_err_to_name(err));
    }
    s_next_alarm = until;
    s_wake_at = until;
    err = pcf85063_set_alarm(until); /* also clears the alarm flag */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC alarm: %s; the backup timer takes over", esp_err_to_name(err));
    }
    time_t wake = until;
    if (board_buttons_pressed(BOARD_BUTTON_KEY) || board_buttons_pressed(BOARD_BUTTON_BOOT)) {
        /* D16 leaves a held button out of this sleep's wake sources: look again soon, so that
         * once it is released, a press can still show the dashboard */
        time_t recheck = time(NULL) + NIGHT_RECHECK_S;
        wake = recheck < wake ? recheck : wake;
    }
    enter_deep_sleep(wake);
}

/* Spec §8: the critical screen stays up, and only KEY wakes the chip to check the battery again. */
static void enter_critical_sleep(void)
{
    app_ui_render();
    if (!prepare_deep_sleep()) {
        handle_wake(power_sleep_light(time(NULL) + CRITICAL_RECHECK_S));
        return;
    }
    power_sleep_critical(CRITICAL_RECHECK_S);
}

static esp_err_t start_rtc_int(void)
{
    /* NM-EPD-420 uses internal ESP32-S3 RTC / timer wakeups; no external RTC INT pin */
    return ESP_OK;
}

/* The RTC alarm or its backup timer: back to sleep in a fraction of a second, unless the board
 * then decides to stay awake. */
static bool routine_wake(power_wake_t wake)
{
    return wake == POWER_WAKE_RTC || wake == POWER_WAKE_TIMER;
}

/* Logs, settings and the console: what a board needs once it stays awake. Routine wakes skip
 * them, because they cost time on every wake and nobody can use them (spec §3.3). */
static bool s_alive; /* come_alive() ran: the board stays awake */

static void come_alive(void)
{
    if (s_alive) {
        return;
    }
    s_alive = true;
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "reflbo %s starting", esp_app_get_description()->version);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        err = power_load_settings();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "power settings: %s", esp_err_to_name(err));
        }
    } else {
        /* Never erase NVS on our own (AGENTS.md quick rule 3): settings fall back to defaults. */
        ESP_LOGE(TAG, "NVS unavailable (%s); settings use their defaults", esp_err_to_name(err));
    }
    app_pc_presets_changed(); /* M7: the charts' history, in PSRAM, now that the board stays awake */
    err = diag_start();
    if (err == ESP_OK) {
        app_register_commands();
    } else {
        ESP_LOGE(TAG, "diagnostics console failed to start: %s", esp_err_to_name(err));
    }
    size_t dump_addr, dump_size; /* spec §16; IDF's own boot check is off, it would run every wake */
    if (esp_core_dump_image_get(&dump_addr, &dump_size) == ESP_OK) {
        ESP_LOGW(TAG, "a %u-byte core dump is in flash; read it with `idf.py coredump-info`", (unsigned)dump_size);
    }
}

void app_alive(void)
{
    come_alive();
}

static void *json_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); /* Wi-Fi and TLS need the internal RAM */
}

static esp_err_t boot(void)
{
    esp_err_t err = power_init();
    power_wake_t wake = power_boot_wake();
    esp_ota_img_states_t ota = ESP_OTA_IMG_UNDEFINED;
    s_ota_pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota) == ESP_OK &&
                    ota == ESP_OTA_IMG_PENDING_VERIFY;
    if (!routine_wake(wake)) {
        come_alive();
    }
    bool resume_config = wake == POWER_WAKE_COLD && resume_config_get(); /* NVS is up on a cold boot */
    if (resume_config && !s_ota_pending) {
        /* Once: a crash in config mode must not bring it back forever. A new image keeps the flag
         * until it is valid, so that after a rollback the old one comes back in config mode too
         * and the web UI can tell. */
        resume_config_set(false);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "power");
    bool snap_ok = util_snapshot_valid(&s_snap, sizeof(s_snap), SNAP_MAGIC, SNAP_VERSION) &&
                   util_snapshot_valid(&s_presets_snap, sizeof(s_presets_snap), PRESETS_MAGIC, PRESETS_VERSION);
    bool warm = wake != POWER_WAKE_COLD && snap_ok;
    if (warm) {
        app_ui_import(&s_snap.ui); /* routine wakes skip storage: everything is in RTC RAM */
        app_ui_import_presets(&s_presets_snap.presets);
        s_next_alarm = s_snap.next_alarm;
    } else {
        app_ui_defaults();
        app_ui_load();
        app_ui_restore_forecast(); /* spec §6: shown as stale by its age */
        app_solar_restore();       /* spec §11.5, §11.6: the forecast and today's readings */
        if (wake == POWER_WAKE_COLD && snap_ok) {
            app_ui_state_t *st = app_state();
            st->ds = s_snap.ui.ds;
            st->cold_boot_at = s_snap.ui.cold_boot_at;
            st->done_slot = s_snap.ui.done_slot;
            if (s_snap.ui.sync.ok_at != 0) {
                st->sync = s_snap.ui.sync;
            }
        }
    }
    app_pc_init(); /* M7: the cached catalogue (a cold boot's), the charts' history from nothing */
    if (s_alive) { /* a cold boot came alive before its presets loaded: their charts' history now */
        app_pc_presets_changed();
    }

    ESP_RETURN_ON_ERROR(board_init(wake == POWER_WAKE_COLD), TAG, "board");
    ESP_RETURN_ON_ERROR(pcf85063_init(board_i2c()), TAG, "RTC");
    if (wake == POWER_WAKE_COLD) {
        err = timekeeping_trim_start(); /* the RTC lost its trim with its power, or a reset kept it: write it */
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RTC trim: %s", esp_err_to_name(err));
        }
    }
    ESP_RETURN_ON_ERROR(timekeeping_init(app_settings()->tz_posix), TAG, "time zone");
    err = timekeeping_load_from_rtc(wake == POWER_WAKE_RTC);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC read: %s", esp_err_to_name(err));
    }
    timekeeping_drift_correct();
    ESP_RETURN_ON_ERROR(sensors_init(board_i2c(), !warm), TAG, "sensors");
    if (!warm) {
        app_ui_restore_learning(); /* D21: a restart keeps a discharge being learned */
    }
    if (warm) {
        sensors_import(&s_snap.sensors);
        sensors_env_t env = sensors_env();
        if (env.time != 0) {
            display_set_ambient_c((int16_t)(env.temp_c100 / 100));
            timekeeping_set_ambient_c100((int16_t)env.temp_c100);
        }
        ESP_RETURN_ON_ERROR(display_init_warm(&s_snap.display), TAG, "display");
        if (s_snap.display.pushed && s_snap.display.partial_count > 0 &&
            s_snap.display.partial_count < 20) {
            app_ui_seed_previous(s_snap.display.partial_count);
        }
    } else if (wake != POWER_WAKE_COLD) {
        /* Woke from deep sleep without a valid snapshot: the panel still runs, don't reset it. It
         * may have been asleep for the night, so wake it anyway (SLPOUT is harmless otherwise). */
        display_state_t fallback = { .variant = PANEL_VARIANT, .asleep = true };
        ESP_RETURN_ON_ERROR(display_init_warm(&fallback), TAG, "display");
        err = display_wake();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel wake: %s", esp_err_to_name(err));
        }
    } else {
        ESP_RETURN_ON_ERROR(display_init(PANEL_VARIANT), TAG, "display");
    }
    app_ui_apply_settings(); /* offsets, freshness and the panel rate, now that the panel is up */
    ESP_RETURN_ON_ERROR(board_buttons_start(on_button, k_app_dashboard_buttons), TAG, "buttons");
    ESP_RETURN_ON_ERROR(start_rtc_int(), TAG, "RTC INT");
    lora_widget_set_rx_notify_cb(on_lora_notify);
    lora_widget_set_rekey_cb(lora_rekey_cb);
    ignore_held_buttons();

    if (!warm) {
        app_sync_schedule(); /* a warm wake keeps the due sync in its snapshot; NVS may not be up then */
    }
    bool button_wake = wake == POWER_WAKE_KEY || wake == POWER_WAKE_BOOT;
    board_button_t woke_by = wake == POWER_WAKE_KEY ? BOARD_BUTTON_KEY : BOARD_BUTTON_BOOT;
    if (wake == POWER_WAKE_COLD || button_wake) {
        power_hold_awake_ms(GRACE_MS);
    }
    if (button_wake && app_ui_night() && time(NULL) < app_state()->night_until) {
        night_peek(woke_by);
    } else {
        if (button_wake && app_state()->critical) {
            board_buttons_ignore_until_released(woke_by); /* it only asks for a battery check */
        } else if (button_wake) {
            board_buttons_woke(woke_by);
        }
        bool rtc_edge = (wake == POWER_WAKE_RTC) || (wake == POWER_WAKE_TIMER && s_next_alarm != 0 && time(NULL) >= s_next_alarm);
        on_tick(!warm || app_state()->critical, rtc_edge);
    }
    s_rendered = true;
    if (resume_config) {
        ESP_LOGI(TAG, "restarted from the web UI: config mode again");
        app_config_enter();
    }
    if (s_ota_pending) { /* spec §10.5: awake, without deep sleep, until it has proved itself */
        ESP_LOGW(TAG, "new firmware: valid after %d s without a panic", OTA_VERIFY_MS / 1000);
    }
    if (app_focus_mode_active()) {
        app_focus_mode_sync(true);
    }
    ESP_LOGI(TAG, "reflbo ready (%s wake%s)", power_wake_name(wake), warm ? ", warm" : "");
    return ESP_OK;
}

/* Spec §10.5: the panel came up and drew, and a minute passed without a panic. */
static void check_ota(void)
{
    if (s_ota_pending && s_rendered && app_uptime_ms() >= OTA_VERIFY_MS) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        s_ota_pending = false;
        resume_config_set(false);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "new firmware marked valid");
        } else {
            ESP_LOGE(TAG, "marking the firmware valid: %s", esp_err_to_name(err));
        }
    }
}

static void app_task(void *arg)
{
    (void)arg;
    esp_err_t err = boot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot failed: %s; the console stays up while a PC is attached, otherwise the "
                 "board sleeps and boots again in %d s", esp_err_to_name(err), RETRY_S);
        power_boot_failed(s_ota_pending);
        power_hold_awake_ms(GRACE_MS);
    }
    for (;;) {
        /* Config mode and a new image waiting to prove itself keep the chip awake: sleep would
         * drop Wi-Fi, and a deep-sleep wake would roll the image back (spec §10.5). */
        bool focus = app_focus_mode_active();
        bool lora_en = app_state() && app_state()->settings.lora.enabled;
        bool lora_wake_on_rx = app_state() && app_state()->settings.lora.wake_on_rx;
        bool lora_wake_lock = power_lora_wake_lock(
            focus, lora_en, lora_wake_on_rx,
            power_tethered(), lora_widget_ble_connected());
        if (lora_wake_lock) {
            lora_widget_resume_ble();
        }
        bool sync_wifi_lock = app_sync_active() || app_sync_wifi_pending() || (!focus && app_sync_holds_wifi());
        bool pending = uxQueueMessagesWaiting(s_queue) > 0 || board_buttons_busy() || app_config_active() ||
                       app_radar_loop_deadline_ms() != 0 ||
                       s_ota_pending || sync_wifi_lock || lora_wake_lock;
        if (err == ESP_OK) {
            check_clock_jump();
            check_ota();
            int64_t mono = app_uptime_ms();
            if (app_menu_is_open() && mono >= app_menu_deadline_ms()) {
                app_menu_close(); /* 60 s without input (spec §5.7) */
            }
            app_config_tick();
            app_sync_wifi_check();
            app_ui_toast_expire();
            app_radar_loop_tick();
            app_pc_tick(); /* M7: a session's silence */
            bool busy = pending || app_menu_is_open() || app_ui_toast_active();
            if (!busy && app_ui_night() && mono >= s_peek_until_ms) {
                enter_night_sleep(); /* returns only if it had to sleep light instead */
                continue;
            }
            if (!busy && app_state()->critical && !power_tethered()) { /* a PC powers it: stay awake (D14) */
                enter_critical_sleep();
                continue;
            }
        }
        switch (power_plan(pending)) {
        case POWER_PLAN_LIGHT:
            handle_wake(power_sleep_light(sleep_until()));
            if ((s_wake_at != 0 && time(NULL) < s_wake_at) && uxQueueMessagesWaiting(s_queue) == 0) {
                continue;
            }
            break;
        case POWER_PLAN_DEEP:
            if (!focus && lora_en && lora_wake_on_rx) {
                handle_wake(power_sleep_light(sleep_until()));
                if ((s_wake_at != 0 && time(NULL) < s_wake_at) && uxQueueMessagesWaiting(s_queue) == 0) {
                    continue;
                }
                break;
            }
            enter_deep_sleep(sleep_until()); /* returns only if it had to sleep light instead */
            continue;
        case POWER_PLAN_RETRY:
            power_sleep_retry(RETRY_S);
            break;
        case POWER_PLAN_ROLLBACK: { /* spec §10.5: the uploaded image failed to start */
            ESP_LOGE(TAG, "the new firmware failed to start; back to the previous one");
            esp_err_t rb = esp_ota_mark_app_invalid_rollback_and_reboot(); /* returns only on failure */
            ESP_LOGE(TAG, "rollback: %s", esp_err_to_name(rb));
            s_ota_pending = false;
            power_boot_failed(false); /* the console or the retry sleep, as for any failed boot */
            break;
        }
        case POWER_PLAN_AWAKE:
            come_alive();
            break;
        }
        int64_t wait_ms = TETHER_RECHECK_MS; /* a failed boot has no schedule to wait for */
        if (err == ESP_OK) {
            int64_t due_ms = (int64_t)sleep_until() * 1000 - now_ms();
            wait_ms = due_ms < wait_ms ? due_ms : wait_ms;
            int64_t mono = app_uptime_ms();
            const int64_t deadlines[] = { app_menu_deadline_ms(), app_ui_toast_until_ms(),
                                          app_ui_night() ? s_peek_until_ms : 0, app_config_redraw_ms(),
                                          s_ota_pending ? OTA_VERIFY_MS : 0, app_radar_loop_deadline_ms(),
                                          app_pc_deadline_ms() };
            for (size_t i = 0; i < sizeof(deadlines) / sizeof(deadlines[0]); i++) {
                if (deadlines[i] != 0 && deadlines[i] - mono < wait_ms) {
                    wait_ms = deadlines[i] - mono;
                }
            }
        }
        TickType_t wait = wait_ms <= 0 ? 0 : util_ticks_at_least((uint32_t)wait_ms, portTICK_PERIOD_MS);
        app_event_t ev;
        if (xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            handle_event(&ev);
        } else if (err == ESP_OK && s_next_alarm != 0 && time(NULL) >= s_next_alarm) {
            on_tick(false, true);
        } else if (err == ESP_OK && time(NULL) >= s_wake_at) {
            on_tick(false, false); /* a cycle switch or seconds tick */
        }
    }
}

esp_err_t app_start(void)
{
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = json_malloc, .free_fn = heap_caps_free });
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(app_event_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreatePinnedToCore(app_task, "app", APP_STACK, NULL, APP_PRIORITY, NULL, APP_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
