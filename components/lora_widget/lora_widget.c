#include "lora_widget.h"
#include "meshcore_proto.h"
#include "meshcore_companion.h"
#include "meshcore_node.h"
#include "meshcore_ble.h"
#include "sx1262_radio.h"
#include "storage.h"
#include "util_base64.h"
#include "util_sha256.h"
#include "timekeeping.h"
#include "ed_25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_attr.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lora_widget";

static lora_stats_t s_stats;
static EXT_RAM_BSS_ATTR meshcore_node_state_t s_node;
static bool s_ble_paused_for_sleep = false;
static SemaphoreHandle_t s_stats_mutex;
static SemaphoreHandle_t s_radio_mutex;
static SemaphoreHandle_t s_companion_mutex;
static SemaphoreHandle_t s_tx_mutex;
static TaskHandle_t s_task;
static lora_config_t s_current_cfg;

static int32_t s_lat_e6 = 0;
static int32_t s_lon_e6 = 0;
static bool s_has_loc = false;

static uint16_t s_batt_mv = 4190;
static uint8_t s_batt_pct = 99;

#define LORA_TX_QUEUE_SIZE 8
typedef struct {
    uint16_t len;
    uint8_t  data[256];
} lora_tx_item_t;

static lora_tx_item_t s_tx_queue[LORA_TX_QUEUE_SIZE];
static size_t s_tx_head = 0;
static size_t s_tx_tail = 0;
static size_t s_tx_count = 0;

static uint8_t s_pub_key[32];
static uint8_t s_prv_key[64];
static bool s_keys_initialized = false;
static RTC_DATA_ATTR uint8_t s_identity_seed[32];
static RTC_DATA_ATTR bool s_has_custom_seed = false;
static RTC_DATA_ATTR uint32_t s_default_ble_pin = 0;
static RTC_DATA_ATTR bool s_legacy_identity = false;
static bool (*s_rekey_cb)(void) = NULL;

#define COMPANION_PUSH_QUEUE_SIZE 8
typedef struct {
    uint8_t len;
    uint8_t data[COMPANION_MAX_FRAME_SIZE];
} companion_frame_t;

static EXT_RAM_BSS_ATTR companion_context_t s_companion_ctx;
static bool s_companion_initialized = false;
static companion_frame_t s_push_queue[COMPANION_PUSH_QUEUE_SIZE];
static size_t s_push_head = 0;
static size_t s_push_tail = 0;
static size_t s_push_count = 0;
static SemaphoreHandle_t s_push_mutex = NULL;

#define COMPANION_MSG_QUEUE_SIZE 32
static companion_frame_t s_msg_queue[COMPANION_MSG_QUEUE_SIZE];
static size_t s_msg_head = 0;
static size_t s_msg_tail = 0;
static size_t s_msg_count = 0;
static SemaphoreHandle_t s_msg_mutex = NULL;

static bool stats_lock(TickType_t timeout_ticks)
{
    if (s_stats_mutex == NULL) {
        s_stats_mutex = xSemaphoreCreateRecursiveMutex();
    }
    return s_stats_mutex && (xSemaphoreTakeRecursive(s_stats_mutex, timeout_ticks) == pdTRUE);
}

static void stats_unlock(void)
{
    if (s_stats_mutex) {
        xSemaphoreGiveRecursive(s_stats_mutex);
    }
}

static bool companion_lock(TickType_t timeout_ticks)
{
    if (s_companion_mutex == NULL) {
        s_companion_mutex = xSemaphoreCreateRecursiveMutex();
    }
    return s_companion_mutex && (xSemaphoreTakeRecursive(s_companion_mutex, timeout_ticks) == pdTRUE);
}

static void companion_unlock(void)
{
    if (s_companion_mutex) {
        xSemaphoreGiveRecursive(s_companion_mutex);
    }
}

#define LORA_CHANNELS_PATH "/fs/state/lora_channels.bin"
#define LORA_CHANNELS_MAGIC 0x4C524348 /* "LRCH" */
#define LORA_CHANNELS_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    companion_channel_t channels[COMPANION_MAX_CHANNELS];
} lora_channels_file_t;

#define LORA_CONTACTS_PATH "/fs/state/lora_contacts.bin"
#define LORA_CONTACTS_MAGIC 0x4C524354 /* "LRCT" */
#define LORA_CONTACTS_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint16_t contact_count;
    uint16_t reserved;
    companion_contact_t contacts[COMPANION_MAX_CONTACTS];
} lora_contacts_file_t;

static EXT_RAM_BSS_ATTR lora_channels_file_t s_channels_file_buf;
static EXT_RAM_BSS_ATTR lora_contacts_file_t s_contacts_file_buf;
static bool s_contacts_dirty = false;
static uint32_t s_contacts_dirty_since_ms = 0;
#define LORA_CONTACTS_FLUSH_DELAY_MS 30000U

#define LORA_MSGS_PATH "/fs/state/lora_msgs.bin"
#define LORA_MSGS_MAGIC 0x4C524D53U /* "LRMS" */
#define LORA_MSGS_VERSION 1U
#define LORA_MSGS_FLUSH_DELAY_MS 30000U
#define LORA_DM_ACK_TIMEOUT_MS 15000U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t next_msg_id;
    uint16_t msg_count;
    uint16_t reserved;
    meshcore_msg_t messages[MESHCORE_MAX_MESSAGES];
} lora_msgs_file_t;

static EXT_RAM_BSS_ATTR lora_msgs_file_t s_msgs_file_buf;
static bool s_companion_fs_loaded = false;
static bool s_msgs_fs_loaded = false;
static bool s_msgs_dirty = false;
static uint32_t s_msgs_dirty_since_ms = 0;
static SemaphoreHandle_t s_msgs_io_mutex = NULL;

static void (*s_rx_notify_cb)(void) = NULL;

void lora_widget_set_rx_notify_cb(void (*cb)(void))
{
    s_rx_notify_cb = cb;
}

static bool read_binary_with_backup(const char *path, void *buf, size_t size,
                                    uint32_t expected_magic, uint32_t expected_version)
{
    if (!path || !buf || size < 8) return false;

    bool main_corrupt = false;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(buf, size, 1, f);
        fclose(f);
        const uint32_t *hdr = (const uint32_t *)buf;
        if (n == 1 && hdr[0] == expected_magic && hdr[1] == expected_version) {
            return true;
        }
        main_corrupt = true;
        ESP_LOGW(TAG, "Invalid or corrupt file in %s, trying .bak", path);
    }

    char bak_path[STORAGE_PATH_MAX];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
    FILE *fb = fopen(bak_path, "rb");
    if (!fb) {
        return false;
    }
    size_t n = fread(buf, size, 1, fb);
    fclose(fb);
    const uint32_t *hdr = (const uint32_t *)buf;
    if (n != 1 || hdr[0] != expected_magic || hdr[1] != expected_version) {
        ESP_LOGW(TAG, "Backup file invalid in %s", bak_path);
        return false;
    }
    if (main_corrupt) {
        /* Remove corrupt primary so the next atomic write won't overwrite the valid .bak */
        unlink(path);
    }
    ESP_LOGW(TAG, "Recovered %s from backup %s", path, bak_path);
    return true;
}

static void save_channels_to_storage(const companion_context_t *ctx)
{
    if (!ctx || !storage_ready()) return;
    s_channels_file_buf.magic = LORA_CHANNELS_MAGIC;
    s_channels_file_buf.version = LORA_CHANNELS_VERSION;
    memcpy(s_channels_file_buf.channels, ctx->channels, sizeof(s_channels_file_buf.channels));
    esp_err_t err = storage_write_atomic(LORA_CHANNELS_PATH,
                                         (const char *)&s_channels_file_buf,
                                         sizeof(s_channels_file_buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed atomic save of %s (%s)", LORA_CHANNELS_PATH, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "LoRa channels saved atomically to storage");
}

static bool load_channels_from_storage(companion_context_t *ctx)
{
    if (!ctx) return false;
    if (!read_binary_with_backup(LORA_CHANNELS_PATH, &s_channels_file_buf, sizeof(s_channels_file_buf),
                                 LORA_CHANNELS_MAGIC, LORA_CHANNELS_VERSION)) {
        return false;
    }
    memcpy(ctx->channels, s_channels_file_buf.channels, sizeof(ctx->channels));
    ESP_LOGI(TAG, "LoRa channels loaded from storage");
    return true;
}

static void on_companion_channels_changed(companion_context_t *ctx, void *user_data)
{
    (void)user_data;
    save_channels_to_storage(ctx);
}

static void save_contacts_to_storage(const companion_context_t *ctx)
{
    if (!ctx || !storage_ready()) return;
    s_contacts_file_buf.magic = LORA_CONTACTS_MAGIC;
    s_contacts_file_buf.version = LORA_CONTACTS_VERSION;
    s_contacts_file_buf.contact_count = (uint16_t)ctx->contact_count;
    s_contacts_file_buf.reserved = 0;
    memcpy(s_contacts_file_buf.contacts, ctx->contacts, sizeof(s_contacts_file_buf.contacts));
    esp_err_t err = storage_write_atomic(LORA_CONTACTS_PATH,
                                         (const char *)&s_contacts_file_buf,
                                         sizeof(s_contacts_file_buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed atomic save of %s (%s)", LORA_CONTACTS_PATH, esp_err_to_name(err));
        return;
    }
    s_contacts_dirty = false;
    ESP_LOGI(TAG, "LoRa contacts saved atomically to storage (%u contacts)", (unsigned)ctx->contact_count);
}

static void flush_dirty_contacts_if_due(bool force, uint32_t now_ms)
{
    if (!s_contacts_dirty) return;
    if (!force && (uint32_t)(now_ms - s_contacts_dirty_since_ms) < LORA_CONTACTS_FLUSH_DELAY_MS) {
        return;
    }
    if (companion_lock(pdMS_TO_TICKS(force ? 500 : 50))) {
        if (s_contacts_dirty && s_companion_initialized) {
            uint32_t cur_ms = now_ms ? now_ms : (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (force || (uint32_t)(cur_ms - s_contacts_dirty_since_ms) >= LORA_CONTACTS_FLUSH_DELAY_MS) {
                save_contacts_to_storage(&s_companion_ctx);
            }
        }
        companion_unlock();
    }
}

static bool load_contacts_from_storage(companion_context_t *ctx)
{
    if (!ctx) return false;
    if (!read_binary_with_backup(LORA_CONTACTS_PATH, &s_contacts_file_buf, sizeof(s_contacts_file_buf),
                                 LORA_CONTACTS_MAGIC, LORA_CONTACTS_VERSION)) {
        return false;
    }
    size_t cnt = s_contacts_file_buf.contact_count;
    if (cnt > COMPANION_MAX_CONTACTS) cnt = COMPANION_MAX_CONTACTS;
    ctx->contact_count = cnt;
    memcpy(ctx->contacts, s_contacts_file_buf.contacts, sizeof(ctx->contacts));
    ESP_LOGI(TAG, "LoRa contacts loaded from storage (%u contacts)", (unsigned)cnt);
    return true;
}

static void on_companion_contacts_changed(companion_context_t *ctx, void *user_data)
{
    (void)user_data;
    save_contacts_to_storage(ctx);
}

static void remove_advert_by_pubkey(const uint8_t *pub_key)
{
    if (!pub_key) return;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        meshcore_node_remove_advert_by_pubkey(&s_node, &s_stats, pub_key);
        stats_unlock();
    }
}

static void on_companion_contact_removed(const uint8_t *pub_key, void *user_data)
{
    (void)user_data;
    remove_advert_by_pubkey(pub_key);
}

#define BLE_CMD_QUEUE_SIZE 8
typedef struct {
    uint16_t len;
    uint8_t  data[256];
} ble_cmd_frame_t;

static ble_cmd_frame_t s_ble_cmd_queue[BLE_CMD_QUEUE_SIZE];
static size_t s_ble_cmd_head = 0;
static size_t s_ble_cmd_tail = 0;
static size_t s_ble_cmd_count = 0;
static SemaphoreHandle_t s_ble_cmd_mutex = NULL;

/* Lock order: s_companion_mutex -> s_stats_mutex -> s_tx_mutex; independently s_radio_mutex -> s_stats_mutex. */
static void init_sync_primitives(void)
{
    if (s_stats_mutex == NULL) {
        s_stats_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_radio_mutex == NULL) {
        s_radio_mutex = xSemaphoreCreateMutex();
    }
    if (s_companion_mutex == NULL) {
        s_companion_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutex();
    }
    if (s_ble_cmd_mutex == NULL) {
        s_ble_cmd_mutex = xSemaphoreCreateMutex();
    }
    if (s_push_mutex == NULL) {
        s_push_mutex = xSemaphoreCreateMutex();
    }
    if (s_msg_mutex == NULL) {
        s_msg_mutex = xSemaphoreCreateMutex();
    }
    if (s_msgs_io_mutex == NULL) {
        s_msgs_io_mutex = xSemaphoreCreateMutex();
    }
}

static void ensure_companion_loaded(void)
{
    init_sync_primitives();
    if (!companion_lock(pdMS_TO_TICKS(200))) {
        return;
    }
    if (!s_companion_initialized) {
        meshcore_companion_init(&s_companion_ctx);
        s_companion_initialized = true;
    }
    if (!s_companion_fs_loaded && storage_ready()) {
        load_channels_from_storage(&s_companion_ctx);
        load_contacts_from_storage(&s_companion_ctx);
        s_companion_fs_loaded = true;
    }
    companion_unlock();
}

static void sync_slot0_channel_from_cfg(const lora_config_t *cfg_local)
{
    if (!cfg_local) return;
    ensure_companion_loaded();

    uint8_t secret32[32] = {0};
    uint8_t ch_hash = 0;
    if (!meshcore_channel_key_from_psk(cfg_local->channel_psk, secret32, &ch_hash)) {
        return;
    }

    if (companion_lock(pdMS_TO_TICKS(100))) {
        companion_channel_t *ch0 = &s_companion_ctx.channels[0];
        bool changed = !ch0->valid ||
                       strncmp(ch0->name, cfg_local->channel_name, sizeof(ch0->name)) != 0 ||
                       memcmp(ch0->secret, secret32, 16) != 0;
        if (changed) {
            memset(ch0->name, 0, sizeof(ch0->name));
            snprintf(ch0->name, sizeof(ch0->name), "%s", cfg_local->channel_name);
            memcpy(ch0->secret, secret32, 16);
            ch0->valid = true;
            if (storage_ready()) {
                save_channels_to_storage(&s_companion_ctx);
            }
        }
        companion_unlock();
    }
}

static void ensure_node_msgs_loaded(void)
{
    init_sync_primitives();
    if (s_msgs_fs_loaded || !storage_ready()) {
        if (stats_lock(pdMS_TO_TICKS(50))) {
            if (s_node.next_msg_id == 0) {
                s_node.next_msg_id = 1U;
            }
            stats_unlock();
        }
        return;
    }

    if (s_msgs_io_mutex && xSemaphoreTake(s_msgs_io_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (!s_msgs_fs_loaded) {
            bool ok = read_binary_with_backup(LORA_MSGS_PATH, &s_msgs_file_buf, sizeof(s_msgs_file_buf),
                                              LORA_MSGS_MAGIC, LORA_MSGS_VERSION);
            if (stats_lock(pdMS_TO_TICKS(200))) {
                if (!s_msgs_fs_loaded && ok && s_stats.msg_count == 0) {
                    size_t cnt = s_msgs_file_buf.msg_count;
                    if (cnt > MESHCORE_MAX_MESSAGES) cnt = MESHCORE_MAX_MESSAGES;
                    for (size_t i = 0; i < cnt; i++) {
                        s_node.messages[i] = s_msgs_file_buf.messages[i];
                        if (s_node.messages[i].ack_status == MESHCORE_ACK_PENDING) {
                            s_node.messages[i].ack_status = MESHCORE_ACK_TIMEOUT;
                        }
                    }
                    s_stats.msg_count = (uint8_t)cnt;
                    uint32_t next_id = s_msgs_file_buf.next_msg_id;
                    if (next_id < (uint32_t)cnt + 1U) {
                        next_id = (uint32_t)cnt + 1U;
                    }
                    s_node.next_msg_id = next_id;
                    if (cnt > 0) {
                        s_stats.msg_seq++;
                    }
                    ESP_LOGI(TAG, "LoRa messages loaded from storage (%u msgs)", (unsigned)cnt);
                } else if (s_node.next_msg_id == 0) {
                    s_node.next_msg_id = 1U;
                }
                s_msgs_fs_loaded = true;
                stats_unlock();
            }
        }
        xSemaphoreGive(s_msgs_io_mutex);
    }
}

static void mark_msgs_dirty_locked(uint32_t now_ms)
{
    if (!s_msgs_dirty) {
        s_msgs_dirty = true;
        s_msgs_dirty_since_ms = now_ms ? now_ms : (uint32_t)(esp_timer_get_time() / 1000ULL);
    }
}

static void flush_dirty_msgs_if_due(bool force, uint32_t now_ms)
{
    if (!s_msgs_dirty || !storage_ready()) return;
    uint32_t cur_ms = now_ms ? now_ms : (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (!force && (uint32_t)(cur_ms - s_msgs_dirty_since_ms) < LORA_MSGS_FLUSH_DELAY_MS) {
        return;
    }

    if (!s_msgs_io_mutex || xSemaphoreTake(s_msgs_io_mutex, pdMS_TO_TICKS(force ? 500 : 50)) != pdTRUE) {
        return;
    }

    bool should_write = false;
    if (stats_lock(pdMS_TO_TICKS(force ? 200 : 50))) {
        if (s_msgs_dirty) {
            cur_ms = now_ms ? now_ms : (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (force || (uint32_t)(cur_ms - s_msgs_dirty_since_ms) >= LORA_MSGS_FLUSH_DELAY_MS) {
                memset(&s_msgs_file_buf, 0, sizeof(s_msgs_file_buf));
                s_msgs_file_buf.magic = LORA_MSGS_MAGIC;
                s_msgs_file_buf.version = LORA_MSGS_VERSION;
                s_msgs_file_buf.next_msg_id = s_node.next_msg_id ? s_node.next_msg_id : 1U;
                uint16_t cnt = s_stats.msg_count;
                if (cnt > MESHCORE_MAX_MESSAGES) cnt = MESHCORE_MAX_MESSAGES;
                s_msgs_file_buf.msg_count = cnt;
                if (cnt > 0) {
                    memcpy(s_msgs_file_buf.messages, s_node.messages, cnt * sizeof(meshcore_msg_t));
                }
                s_msgs_dirty = false;
                should_write = true;
            }
        }
        stats_unlock();
    }

    if (should_write) {
        esp_err_t err = storage_write_atomic(LORA_MSGS_PATH,
                                             (const char *)&s_msgs_file_buf,
                                             sizeof(s_msgs_file_buf));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed atomic save of %s (%s)", LORA_MSGS_PATH, esp_err_to_name(err));
            if (stats_lock(pdMS_TO_TICKS(50))) {
                mark_msgs_dirty_locked(cur_ms);
                stats_unlock();
            }
        } else {
            ESP_LOGI(TAG, "LoRa messages saved atomically to storage (%u msgs)", (unsigned)s_msgs_file_buf.msg_count);
        }
    }

    xSemaphoreGive(s_msgs_io_mutex);
}

static void tick_ack_timeout_and_flush(uint32_t now_ms)
{
    bool timed_out = false;
    if (stats_lock(pdMS_TO_TICKS(20))) {
        if (meshcore_node_expire_acks(&s_node, &s_stats, now_ms, LORA_DM_ACK_TIMEOUT_MS) > 0U) {
            mark_msgs_dirty_locked(now_ms);
            timed_out = true;
        }
        stats_unlock();
    }
    if (timed_out && s_rx_notify_cb) {
        s_rx_notify_cb();
    }

    flush_dirty_contacts_if_due(false, now_ms);
    flush_dirty_msgs_if_due(false, now_ms);
}

static esp_err_t enqueue_tx_packet(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > sizeof(s_tx_queue[0].data)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutex();
    }
    if (!s_tx_mutex || xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (s_tx_count < LORA_TX_QUEUE_SIZE) {
        s_tx_queue[s_tx_tail].len = (uint16_t)len;
        memcpy(s_tx_queue[s_tx_tail].data, data, len);
        s_tx_tail = (s_tx_tail + 1) % LORA_TX_QUEUE_SIZE;
        s_tx_count++;
    } else {
        err = ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_tx_mutex);
    if (err == ESP_OK && s_task) {
        xTaskNotifyGive(s_task);
    }
    return err;
}

static void on_ble_rx(const uint8_t *data, size_t len);
static void push_companion_event(const uint8_t *data, size_t len);
static bool companion_pop_msg(uint8_t *msg_out, size_t max, size_t *out_len);
static uint32_t get_default_ble_pin(void);

/* Forward declarations */
static esp_err_t sx1262_transmit(const uint8_t *data, size_t len);

static void init_node_keys(void)
{
    if (s_keys_initialized) {
        return;
    }

    uint8_t seed[32];
    if (s_has_custom_seed) {
        memcpy(seed, s_identity_seed, sizeof(seed));
        s_stats.legacy_identity = false;
    } else {
        uint8_t mac[6] = {0};
        esp_err_t err = esp_efuse_mac_get_default(mac);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_efuse_mac_get_default failed (%s), using random seed", esp_err_to_name(err));
            esp_fill_random(mac, sizeof(mac));
        }

        /* Legacy Seed = SHA256("meshcore-ed25519-v1" || mac[6]) */
        uint8_t seed_input[19 + 6];
        memcpy(seed_input, "meshcore-ed25519-v1", 19);
        memcpy(seed_input + 19, mac, 6);
        util_sha256(seed_input, sizeof(seed_input), seed);
        s_stats.legacy_identity = s_legacy_identity;
    }

    meshcore_ed25519_create_keypair(s_pub_key, s_prv_key, seed);
    memset(seed, 0, sizeof(seed));

    for (int i = 0; i < 32; i++) {
        snprintf(s_stats.pub_key_hex + (i * 2), 3, "%02x", s_pub_key[i]);
    }
    s_stats.pub_key_hex[64] = '\0';
    s_keys_initialized = true;

    ESP_LOGI(TAG, "MeshCore Ed25519 Node Key %s. PubKey prefix: %.16s...",
             s_has_custom_seed ? "loaded from NVS" : "derived from MAC (legacy)", s_stats.pub_key_hex);
}

static uint32_t get_default_ble_pin(void)
{
    if (s_default_ble_pin >= 100000u && s_default_ble_pin <= 999999u) {
        return s_default_ble_pin;
    }
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    return 100000u + ((((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5]) % 900000u);
}

void lora_widget_set_identity(const uint8_t seed[32], uint32_t default_pin, bool legacy)
{
    init_sync_primitives();
    if (stats_lock(pdMS_TO_TICKS(100))) {
        if (seed != NULL) {
            memcpy(s_identity_seed, seed, 32);
            s_has_custom_seed = true;
        } else {
            memset(s_identity_seed, 0, sizeof(s_identity_seed));
            s_has_custom_seed = false;
        }
        s_default_ble_pin = (default_pin >= 100000u && default_pin <= 999999u) ? default_pin : 0u;
        s_legacy_identity = legacy;
        s_stats.legacy_identity = legacy;
        s_keys_initialized = false;
        init_node_keys();
        s_stats.ble_pin = (s_current_cfg.ble_pin >= 100000u && s_current_cfg.ble_pin <= 999999u)
                              ? s_current_cfg.ble_pin
                              : get_default_ble_pin();
        stats_unlock();
    }
}

void lora_widget_set_rekey_cb(bool (*cb)(void))
{
    s_rekey_cb = cb;
}

static esp_err_t sx1262_transmit(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    bool ready = false;
    bool cad_enabled = false;
    uint8_t airtime_factor_x10 = 10;
    uint8_t sf = 7;
    uint8_t cr = 5;
    uint32_t bw_hz = 125000;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t wait_ms = 0;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        ready = s_stats.initialized && s_stats.enabled && sx1262_radio_is_ready();
        cad_enabled = s_current_cfg.cad_enabled;
        airtime_factor_x10 = s_current_cfg.airtime_factor_x10;
        sf = s_current_cfg.sf;
        cr = s_current_cfg.cr;
        bw_hz = s_current_cfg.bw_hz ? s_current_cfg.bw_hz : (uint32_t)s_current_cfg.bw_khz * 1000U;
        wait_ms = meshcore_node_tx_wait_ms(&s_node, now_ms);
        stats_unlock();
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (wait_ms > 0U) {
        if (wait_ms > 500U) {
            ESP_LOGW(TAG, "TX airtime budget busy (%lu ms remaining)", (unsigned long)wait_ms);
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    if (!s_radio_mutex || xSemaphoreTake(s_radio_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool still_ready = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        still_ready = s_stats.initialized && s_stats.enabled && sx1262_radio_is_ready();
        stats_unlock();
    }
    if (!still_ready) {
        xSemaphoreGive(s_radio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t air_ms = 0;
    esp_err_t err = sx1262_radio_transmit(data, len, bw_hz, sf, cr, cad_enabled, &air_ms, s_task);
    xSemaphoreGive(s_radio_mutex);

    if (err == ESP_OK) {
        uint8_t route = data[0] & MESHCORE_PH_ROUTE_MASK;
        uint32_t done_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint32_t total_air_ms = 0;
        if (stats_lock(pdMS_TO_TICKS(50))) {
            s_stats.packets_sent++;
            if (route == MESHCORE_ROUTE_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_FLOOD) {
                s_stats.sent_flood++;
            } else {
                s_stats.sent_direct++;
            }
            s_stats.tx_air_ms += air_ms;
            meshcore_node_record_tx_airtime(&s_node, done_ms, air_ms, airtime_factor_x10);
            total_air_ms = s_stats.tx_air_ms;
            stats_unlock();
        }
        ESP_LOGI(TAG, "TX done: %u bytes, airtime: %lu ms (total TX air: %lu ms)",
                 (unsigned)len, (unsigned long)air_ms, (unsigned long)total_air_ms);
    }
    return err;
}

bool lora_widget_get_advert(size_t index, meshcore_advert_t *out)
{
    if (out == NULL) return false;
    bool ok = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        if (index < s_stats.advert_count && index < MESHCORE_MAX_ADVERTS) {
            *out = s_node.adverts[index];
            ok = true;
        }
        stats_unlock();
    }
    return ok;
}

bool lora_widget_get_neighbor(size_t index, meshcore_neighbor_t *out)
{
    if (out == NULL) return false;
    bool ok = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        if (index < s_stats.neighbor_count && index < MESHCORE_MAX_NEIGHBORS) {
            *out = s_node.neighbors[index];
            ok = true;
        }
        stats_unlock();
    }
    return ok;
}

bool lora_widget_get_message(size_t index, meshcore_msg_t *out)
{
    if (out == NULL) return false;
    bool ok = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        if (index < s_stats.msg_count && index < MESHCORE_MAX_MESSAGES) {
            *out = s_node.messages[index];
            ok = true;
        }
        stats_unlock();
    }
    return ok;
}

static void drain_tx_queue(void)
{
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint32_t wait_ms = 0;
        if (stats_lock(pdMS_TO_TICKS(50))) {
            wait_ms = meshcore_node_tx_wait_ms(&s_node, now_ms);
            stats_unlock();
        }
        if (wait_ms > 200U) {
            break;
        }

        lora_tx_item_t item;
        bool have_item = false;
        if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_tx_count > 0) {
                item = s_tx_queue[s_tx_head];
                s_tx_head = (s_tx_head + 1) % LORA_TX_QUEUE_SIZE;
                s_tx_count--;
                have_item = true;
            }
            xSemaphoreGive(s_tx_mutex);
        }
        if (!have_item) {
            break;
        }
        esp_err_t err = sx1262_transmit(item.data, item.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Queued TX failed (%s, %u bytes)", esp_err_to_name(err), (unsigned)item.len);
        }
    }
}

static void fill_companion_env(companion_env_t *env);
static bool companion_send_advert(bool flood);
static bool companion_send_channel_txt(uint8_t channel_idx, const char *text);
static bool companion_send_direct_txt(const uint8_t pub_key[32], uint8_t txt_type, uint32_t msg_ts,
                                      const char *text, uint8_t out_ack_tag[4]);
static void companion_set_latlon(int32_t lat_e6, int32_t lon_e6, void *user_data);
static void companion_exec_cli(const char *cmd, char *reply, size_t reply_sz);
static void companion_send_frame_cb(const uint8_t *frame, size_t len, void *user_data);

static void lora_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LoRa SX1262 MeshCore monitor task started (interrupt-driven)");

    for (;;) {
        bool radio_ready = false;
        bool iter_active = false;
        bool have_queued_tx = false;
        uint32_t tx_wait_ms = 0;
        if (stats_lock(pdMS_TO_TICKS(50))) {
            radio_ready = s_stats.enabled && s_stats.initialized;
            if (radio_ready) {
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                tx_wait_ms = meshcore_node_tx_wait_ms(&s_node, now_ms);
            }
            stats_unlock();
        }
        if (companion_lock(pdMS_TO_TICKS(50))) {
            iter_active = s_companion_ctx.contact_iter_active;
            companion_unlock();
        }
        if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            have_queued_tx = (s_tx_count > 0);
            xSemaphoreGive(s_tx_mutex);
        }

        /* Dynamic wait: portMAX_DELAY if radio off, 25ms during active iterator, 2000ms idle */
        TickType_t wait_ticks;
        if (!radio_ready) {
            wait_ticks = portMAX_DELAY;
        } else if (iter_active) {
            wait_ticks = pdMS_TO_TICKS(25);
        } else {
            wait_ticks = pdMS_TO_TICKS(2000);
        }
        if (radio_ready && have_queued_tx) {
            uint32_t wake_ms = (tx_wait_ms > 0U) ? tx_wait_ms : 10U;
            TickType_t q_ticks = pdMS_TO_TICKS(wake_ms);
            if (q_ticks == 0) q_ticks = 1;
            if (q_ticks < wait_ticks) {
                wait_ticks = q_ticks;
            }
        }
        uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);

        /* Step contact iterator if active */
        if (companion_lock(pdMS_TO_TICKS(50))) {
            if (s_companion_ctx.contact_iter_active) {
                companion_env_t env;
                fill_companion_env(&env);
                meshcore_companion_step_iter(&s_companion_ctx, &env);
            }
            companion_unlock();
        }

        /* Drain pending BLE companion commands */
        for (;;) {
            ble_cmd_frame_t frame;
            bool have_frame = false;
            if (s_ble_cmd_mutex && xSemaphoreTake(s_ble_cmd_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (s_ble_cmd_count > 0) {
                    frame = s_ble_cmd_queue[s_ble_cmd_head];
                    s_ble_cmd_head = (s_ble_cmd_head + 1) % BLE_CMD_QUEUE_SIZE;
                    s_ble_cmd_count--;
                    have_frame = true;
                }
                xSemaphoreGive(s_ble_cmd_mutex);
            }
            if (!have_frame) break;

            bool cfg_changed = false;
            lora_config_t cfg_copy;
            memset(&cfg_copy, 0, sizeof(cfg_copy));
            if (stats_lock(pdMS_TO_TICKS(50))) {
                cfg_copy = s_current_cfg;
                stats_unlock();
            }
            uint8_t resp[COMPANION_MAX_FRAME_SIZE];
            size_t resp_len = lora_widget_handle_companion_cmd(frame.data, frame.len, &cfg_copy, &cfg_changed, resp, sizeof(resp));
            if (resp_len > 0) {
                meshcore_ble_send(resp, resp_len);
            }
            drain_tx_queue();
            if (cfg_changed) {
                lora_widget_apply(&cfg_copy);
            }
        }
        drain_tx_queue();

        radio_ready = false;
        if (stats_lock(pdMS_TO_TICKS(50))) {
            radio_ready = s_stats.enabled && s_stats.initialized;
            stats_unlock();
        }
        if (!radio_ready) {
            continue;
        }

        uint32_t loop_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (notified == 0 && gpio_get_level(BOARD_PIN_LORA_DIO1) == 0) {
            static uint32_t s_last_nf_ms = 0;
            bool nf_unsampled = false;
            if (stats_lock(pdMS_TO_TICKS(10))) {
                nf_unsampled = (s_stats.noise_floor_dbm == 0);
                stats_unlock();
            }
            uint32_t nf_interval_ms = meshcore_ble_is_connected() ? 4000U : 12000U;
            if (nf_unsampled || (uint32_t)(loop_ms - s_last_nf_ms) >= nf_interval_ms) {
                s_last_nf_ms = loop_ms;
                int16_t inst = 0;
                bool got_rssi = false;
                if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    got_rssi = sx1262_radio_read_inst_rssi(&inst);
                    xSemaphoreGive(s_radio_mutex);
                }
                if (got_rssi && stats_lock(pdMS_TO_TICKS(20))) {
                    if (s_stats.noise_floor_dbm == 0) {
                        s_stats.noise_floor_dbm = inst;
                    } else {
                        s_stats.noise_floor_dbm = (int16_t)((s_stats.noise_floor_dbm * 7 + inst) / 8);
                    }
                    stats_unlock();
                }
            }
            tick_ack_timeout_and_flush(loop_ms);
            continue;
        }

        /* Read IRQ status, packet metadata, and FIFO under s_radio_mutex */
        static uint8_t s_rx_bytes[256];
        uint16_t irq = 0;
        int16_t rssi = -120;
        int8_t snr_q4 = 0;
        uint8_t payload_len = 0;
        bool irq_ok = false;
        bool read_ok = false;

        if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            irq_ok = sx1262_radio_poll_rx(&irq, s_rx_bytes, &payload_len, &rssi, &snr_q4, &read_ok);
            xSemaphoreGive(s_radio_mutex);
        }

        if (!irq_ok) {
            tick_ack_timeout_and_flush(loop_ms);
            continue;
        }

        /* Check CRC Error */
        if (irq & 0x0040) {
            if (stats_lock(pdMS_TO_TICKS(20))) {
                s_stats.recv_errors++;
                stats_unlock();
            }
        }

        /* Bit 1 is RxDone (0x0002) */
        if (irq & 0x0002) {
            if (!read_ok) {
                flush_dirty_contacts_if_due(false, loop_ms);
                continue;
            }

            /* Update stats using current RF config */
            if (stats_lock(pdMS_TO_TICKS(50))) {
                uint32_t bw_hz = s_current_cfg.bw_hz ? s_current_cfg.bw_hz : (uint32_t)s_current_cfg.bw_khz * 1000;
                uint16_t preamble = (s_current_cfg.sf <= 8) ? 32 : 16;
                uint32_t air_ms = meshcore_calc_airtime_ms(bw_hz, s_current_cfg.sf, s_current_cfg.cr, preamble, payload_len);
                s_stats.packet_count++;
                s_stats.last_rssi_dbm = rssi;
                s_stats.last_snr_q4 = snr_q4;
                s_stats.last_snr_db = snr_q4 / 4;
                s_stats.last_packet_time_s = (uint32_t)time(NULL);
                s_stats.rx_air_ms += air_ms;
                stats_unlock();
            }

            /* Parse MeshCore packet */
            static meshcore_packet_t s_pkt;
            if (!meshcore_packet_decode(&s_pkt, s_rx_bytes, payload_len)) {
                ESP_LOGD(TAG, "Non-MeshCore packet received (%u bytes)", (unsigned)payload_len);
                flush_dirty_contacts_if_due(false, loop_ms);
                continue;
            }

            uint8_t route = meshcore_packet_get_route_type(&s_pkt);
            uint8_t ptype = meshcore_packet_get_payload_type(&s_pkt);
            uint8_t hops = meshcore_packet_get_hop_count(&s_pkt);

#ifdef ESP_PLATFORM
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
#else
            uint32_t now_ms = (uint32_t)(time(NULL) * 1000ULL);
#endif
            bool seen = false;
            if (stats_lock(pdMS_TO_TICKS(50))) {
                seen = meshcore_node_is_seen_and_mark(&s_node, &s_pkt, now_ms);
                stats_unlock();
            }
            if (seen) {
                ESP_LOGD(TAG, "Dropping duplicate flood packet (hops=%u)", (unsigned)hops);
                flush_dirty_contacts_if_due(false, now_ms);
                continue;
            }

            if (stats_lock(pdMS_TO_TICKS(50))) {
                if (route == MESHCORE_ROUTE_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_FLOOD) {
                    s_stats.recv_flood++;
                } else {
                    s_stats.recv_direct++;
                }
                stats_unlock();
            }

            /* Handle payload types */
            if (ptype == MESHCORE_PAYLOAD_ADVERT) {
                static meshcore_advert_t s_adv;
                if (meshcore_parse_advert_payload(s_pkt.payload, s_pkt.payload_len, &s_adv)) {
                    s_adv.hops = hops;
                    s_adv.rssi_dbm = rssi;
                    s_adv.snr_q4 = snr_q4;
                    s_adv.rx_time_s = (uint32_t)time(NULL);

                    if (stats_lock(pdMS_TO_TICKS(50))) {
                        meshcore_node_update_advert(&s_node, &s_stats, &s_adv);
                        s_stats.advert_seq++;
                        strncpy(s_stats.last_sender, s_adv.name, sizeof(s_stats.last_sender) - 1);
                        s_stats.last_sender[sizeof(s_stats.last_sender) - 1] = '\0';
                        stats_unlock();
                    }
                    if (s_rx_notify_cb) {
                        s_rx_notify_cb();
                    }

                    /* Add/update contact in companion table */
                    uint8_t push_code = 0;
                    if (companion_lock(pdMS_TO_TICKS(100))) {
                        if (!s_companion_initialized) {
                            meshcore_companion_init(&s_companion_ctx);
                            load_channels_from_storage(&s_companion_ctx);
                            load_contacts_from_storage(&s_companion_ctx);
                            s_companion_initialized = true;
                        }

                        companion_contact_t *existing = meshcore_companion_get_contact_mut(&s_companion_ctx, s_adv.pub_key);
                        bool auto_add = ((s_companion_ctx.manual_add_contacts & 1) == 0);

                        if (existing) {
                            existing->type = s_adv.node_type;
                            existing->out_path_len = (int8_t)s_pkt.path_len;
                            size_t c_path_len = (size_t)meshcore_packet_get_hash_size(&s_pkt) * (size_t)hops;
                            if (c_path_len > COMPANION_MAX_PATH_SIZE) c_path_len = COMPANION_MAX_PATH_SIZE;
                            if (hops > 0 && c_path_len > 0) {
                                memcpy(existing->out_path, s_pkt.path, c_path_len);
                            }
                            strncpy(existing->name, s_adv.name, sizeof(existing->name) - 1);
                            existing->last_advert_ts = s_adv.timestamp;
                            existing->lat_e6 = s_adv.has_loc ? s_adv.lat_e6 : 0;
                            existing->lon_e6 = s_adv.has_loc ? s_adv.lon_e6 : 0;
                            existing->lastmod = (uint32_t)time(NULL);
                            if (!s_contacts_dirty) {
                                s_contacts_dirty_since_ms = now_ms;
                                s_contacts_dirty = true;
                            }
                            push_code = PUSH_CODE_ADVERT;
                        } else if (auto_add) {
                            companion_contact_t c;
                            memset(&c, 0, sizeof(c));
                            memcpy(c.pub_key, s_adv.pub_key, COMPANION_PUB_KEY_SIZE);
                            c.type = s_adv.node_type;
                            c.flags = 0;
                            c.out_path_len = (int8_t)s_pkt.path_len;
                            size_t c_path_len = (size_t)meshcore_packet_get_hash_size(&s_pkt) * (size_t)hops;
                            if (c_path_len > COMPANION_MAX_PATH_SIZE) c_path_len = COMPANION_MAX_PATH_SIZE;
                            if (hops > 0 && c_path_len > 0) {
                                memcpy(c.out_path, s_pkt.path, c_path_len);
                            }
                            strncpy(c.name, s_adv.name, sizeof(c.name) - 1);
                            c.last_advert_ts = s_adv.timestamp;
                            c.lat_e6 = s_adv.has_loc ? s_adv.lat_e6 : 0;
                            c.lon_e6 = s_adv.has_loc ? s_adv.lon_e6 : 0;
                            c.lastmod = (uint32_t)time(NULL);
                            meshcore_companion_add_update_contact(&s_companion_ctx, &c);
                            if (!s_contacts_dirty) {
                                s_contacts_dirty_since_ms = now_ms;
                                s_contacts_dirty = true;
                            }
                            push_code = PUSH_CODE_ADVERT;
                        } else {
                            /* Manual add enabled: do not auto-add to contacts, push PUSH_CODE_NEW_ADVERT */
                            push_code = PUSH_CODE_NEW_ADVERT;
                        }
                        companion_unlock();
                    }
                    if (push_code != 0) {
                        uint8_t adv_push[1 + COMPANION_PUB_KEY_SIZE];
                        adv_push[0] = push_code;
                        memcpy(&adv_push[1], s_adv.pub_key, COMPANION_PUB_KEY_SIZE);
                        push_companion_event(adv_push, sizeof(adv_push));
                    }

                    ESP_LOGI(TAG, "Discovered Node Advert: '%s' (type %u, hops %u, RSSI %d dBm)",
                             s_adv.name, s_adv.node_type, s_adv.hops, rssi);
                }
            } else if (ptype == MESHCORE_PAYLOAD_CONTROL) {
                if (s_pkt.payload_len >= 6 && (s_pkt.payload[0] & 0xF0) == 0x80) {
                    /* Discover Request received: reply with Discover Response */
                    uint8_t resp_raw[64];
                    size_t raw_sz = meshcore_node_build_discover_resp(&s_pkt, snr_q4, s_pub_key, resp_raw, sizeof(resp_raw));
                    if (raw_sz > 0) {
                        /* Short randomized delay before response to prevent collisions */
                        vTaskDelay(pdMS_TO_TICKS(10 + (esp_random() % 30)));
                        sx1262_transmit(resp_raw, raw_sz);
                    }
                } else if (s_pkt.payload_len >= 14 && (s_pkt.payload[0] & 0xF0) == 0x90) {
                    /* Discover Response received */
                    meshcore_neighbor_t nb;
                    uint32_t tag = 0;
                    if (meshcore_parse_discover_resp(s_pkt.payload, s_pkt.payload_len, &nb, &tag)) {
                        nb.rssi_dbm = rssi;
                        nb.snr_out_q4 = snr_q4;
                        nb.rx_time_s = (uint32_t)time(NULL);
                        /* Resolve neighbor name from saved contacts first (s_companion_mutex -> s_stats_mutex) */
                        if (companion_lock(pdMS_TO_TICKS(50))) {
                            for (size_t ci = 0; ci < s_companion_ctx.contact_count; ci++) {
                                const companion_contact_t *c = &s_companion_ctx.contacts[ci];
                                if (memcmp(c->pub_key, nb.pub_key_prefix, 8) == 0 && c->name[0] != '\0') {
                                    strncpy(nb.name, c->name, sizeof(nb.name) - 1);
                                    nb.name[sizeof(nb.name) - 1] = '\0';
                                    if (nb.node_type == 0 && c->type != 0) {
                                        nb.node_type = c->type;
                                    }
                                    break;
                                }
                            }
                            companion_unlock();
                        }
                        if (stats_lock(pdMS_TO_TICKS(50))) {
                            if (nb.name[0] == '\0') {
                                for (uint8_t ai = 0; ai < s_stats.advert_count && ai < MESHCORE_MAX_ADVERTS; ai++) {
                                    if (memcmp(s_node.adverts[ai].pub_key, nb.pub_key_prefix, 8) == 0 &&
                                        s_node.adverts[ai].name[0] != '\0') {
                                        strncpy(nb.name, s_node.adverts[ai].name, sizeof(nb.name) - 1);
                                        nb.name[sizeof(nb.name) - 1] = '\0';
                                        break;
                                    }
                                }
                            }
                            meshcore_node_update_neighbor(&s_node, &s_stats, &nb);
                            s_stats.advert_seq++;
                            stats_unlock();
                        }
                        if (s_rx_notify_cb) {
                            s_rx_notify_cb();
                        }
                        ESP_LOGI(TAG, "Discover Response from neighbor: prefix %02x%02x%02x%02x (RSSI %d dBm)",
                                 nb.pub_key_prefix[0], nb.pub_key_prefix[1], nb.pub_key_prefix[2], nb.pub_key_prefix[3], rssi);
                    }
                }
            } else if (ptype == MESHCORE_PAYLOAD_GRP_TXT) {
                uint32_t ts = 0;
                char text[160] = {0};
                char ch_name[17] = {0};
                int matched_ch = -1;

                /* Try all active companion channels 0..7 under s_companion_mutex */
                if (companion_lock(pdMS_TO_TICKS(50))) {
                    for (int ch = 0; ch < COMPANION_MAX_CHANNELS; ch++) {
                        if (s_companion_ctx.channels[ch].valid) {
                            if (meshcore_parse_grp_txt_payload_secret(s_pkt.payload, s_pkt.payload_len,
                                                                     s_companion_ctx.channels[ch].secret,
                                                                     &ts, text, sizeof(text))) {
                                matched_ch = ch;
                                strncpy(ch_name, s_companion_ctx.channels[ch].name, sizeof(ch_name) - 1);
                                ch_name[sizeof(ch_name) - 1] = '\0';
                                break;
                            }
                        }
                    }
                    companion_unlock();
                }

                /* Fallback to s_current_cfg.channel_psk if not matched above */
                if (matched_ch < 0) {
                    char psk_copy[64] = {0};
                    char name_copy[LORA_CHANNEL_NAME_LEN] = {0};
                    if (stats_lock(pdMS_TO_TICKS(50))) {
                        strncpy(psk_copy, s_current_cfg.channel_psk, sizeof(psk_copy) - 1);
                        strncpy(name_copy, s_current_cfg.channel_name, sizeof(name_copy) - 1);
                        stats_unlock();
                    }
                    if (psk_copy[0] != '\0' &&
                        meshcore_parse_grp_txt_payload(s_pkt.payload, s_pkt.payload_len,
                                                       psk_copy, &ts, text, sizeof(text))) {
                        matched_ch = 0;
                        strncpy(ch_name, name_copy, sizeof(ch_name) - 1);
                        ch_name[sizeof(ch_name) - 1] = '\0';
                    }
                }

                if (matched_ch >= 0) {
                    meshcore_msg_t msg = {
                        .timestamp = ts,
                        .rx_time_s = (uint32_t)time(NULL),
                        .rssi_dbm = rssi,
                        .snr_q4 = snr_q4,
                        .hops = hops,
                        .outgoing = false,
                        .is_direct = false,
                        .channel_idx = (uint8_t)matched_ch,
                        .ack_status = MESHCORE_ACK_NONE,
                    };
                    if (ch_name[0] != '\0') {
                        int n = snprintf(msg.text, sizeof(msg.text), "[%s] ", ch_name);
                        if (n > 0 && (size_t)n < sizeof(msg.text)) {
                            strncat(msg.text, text, sizeof(msg.text) - 1 - (size_t)n);
                        }
                    } else {
                        strncpy(msg.text, text, sizeof(msg.text) - 1);
                        msg.text[sizeof(msg.text) - 1] = '\0';
                    }
                    const char *colon_sep = strstr(text, ": ");
                    if (colon_sep) {
                        size_t slen = (size_t)(colon_sep - text);
                        if (slen >= sizeof(msg.sender)) slen = sizeof(msg.sender) - 1;
                        memcpy(msg.sender, text, slen);
                        msg.sender[slen] = '\0';
                        snprintf(msg.body, sizeof(msg.body), "%s", colon_sep + 2);
                    } else {
                        snprintf(msg.body, sizeof(msg.body), "%s", text);
                    }

                    bool is_dup_msg = false;
                    bool notify_msg = false;
                    if (stats_lock(pdMS_TO_TICKS(50))) {
                        is_dup_msg = meshcore_node_is_dup_message(&s_node, &s_stats, ts, msg.rx_time_s, msg.text);
                        if (!is_dup_msg) {
                            meshcore_node_append_message(&s_node, &s_stats, &msg);
                            mark_msgs_dirty_locked(now_ms);
                            notify_msg = true;
                            /* Extract sender name before colon */
                            char *colon = strstr(text, ": ");
                            if (colon) {
                                size_t name_len = colon - text;
                                if (name_len >= sizeof(s_stats.last_sender)) {
                                    name_len = sizeof(s_stats.last_sender) - 1;
                                }
                                memcpy(s_stats.last_sender, text, name_len);
                                s_stats.last_sender[name_len] = '\0';
                            }
                        }
                        stats_unlock();
                    }
                    if (notify_msg && s_rx_notify_cb) {
                        s_rx_notify_cb();
                    }

                    if (is_dup_msg) {
                        ESP_LOGD(TAG, "Dropping duplicate message payload: '%s'", text);
                        flush_dirty_contacts_if_due(false, now_ms);
                        continue;
                    }

                    /* Enqueue message for companion app CMD_SYNC_NEXT_MESSAGE */
                    if (s_msg_mutex == NULL) {
                        s_msg_mutex = xSemaphoreCreateMutex();
                    }
                    if (s_msg_mutex && xSemaphoreTake(s_msg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        if (s_msg_count < COMPANION_MSG_QUEUE_SIZE) {
                            companion_frame_t *mf = &s_msg_queue[s_msg_tail];
                            size_t tlen = strlen(text);
                            if (tlen > 120) tlen = 120;
                            mf->data[0] = RESP_CODE_CHANNEL_MSG_RECV_V3;
                            mf->data[1] = (uint8_t)snr_q4;
                            mf->data[2] = 0;
                            mf->data[3] = 0;
                            mf->data[4] = (uint8_t)matched_ch; /* Active channel index */
                            mf->data[5] = (route == MESHCORE_ROUTE_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_FLOOD) ? hops : 0xFF;
                            mf->data[6] = 0; /* txt_type */
                            mf->data[7] = (uint8_t)(ts & 0xFF);
                            mf->data[8] = (uint8_t)((ts >> 8) & 0xFF);
                            mf->data[9] = (uint8_t)((ts >> 16) & 0xFF);
                            mf->data[10] = (uint8_t)((ts >> 24) & 0xFF);
                            memcpy(&mf->data[11], text, tlen);
                            mf->len = (uint8_t)(11 + tlen);
                            s_msg_tail = (s_msg_tail + 1) % COMPANION_MSG_QUEUE_SIZE;
                            s_msg_count++;
                        }
                        xSemaphoreGive(s_msg_mutex);
                    }

                    /* Tickle companion app via BLE */
                    uint8_t tickle = PUSH_CODE_MSG_WAITING;
                    push_companion_event(&tickle, 1);

                    ESP_LOGI(TAG, "Channel Message (ch %d): '%s'", matched_ch, text);
                }
            } else if (ptype == MESHCORE_PAYLOAD_TXT_MSG) {
                if (s_pkt.payload_len >= 2 + 4 + 1 && s_pkt.payload[0] == s_pub_key[0]) {
                    uint8_t src_hash = s_pkt.payload[1];
                    bool matched = false;
                    uint8_t peer_pub[COMPANION_PUB_KEY_SIZE] = {0};
                    char peer_name[COMPANION_NAME_SIZE] = {0};
                    int8_t peer_out_path_len = -1;
                    uint8_t peer_out_path[COMPANION_MAX_PATH_SIZE] = {0};
                    uint8_t plain[256] = {0};
                    int dec_len = -1;

                    if (companion_lock(pdMS_TO_TICKS(100))) {
                        for (size_t ci = 0; ci < s_companion_ctx.contact_count; ci++) {
                            companion_contact_t *c = &s_companion_ctx.contacts[ci];
                            if (c->pub_key[0] == src_hash) {
                                uint8_t shared_secret[32];
                                ed25519_key_exchange(shared_secret, c->pub_key, s_prv_key);
                                dec_len = meshcore_mac_then_decrypt(shared_secret, plain, sizeof(plain),
                                                                    &s_pkt.payload[2], s_pkt.payload_len - 2);
                                if (dec_len > 5) {
                                    matched = true;
                                    c->lastmod = (uint32_t)time(NULL);
                                    memcpy(peer_pub, c->pub_key, sizeof(peer_pub));
                                    strncpy(peer_name, c->name, sizeof(peer_name) - 1);
                                    peer_name[sizeof(peer_name) - 1] = '\0';
                                    peer_out_path_len = c->out_path_len;
                                    if (peer_out_path_len != -1) {
                                        uint8_t pl = (uint8_t)peer_out_path_len;
                                        uint8_t hash_size = (uint8_t)((pl >> 6) + 1U);
                                        size_t path_bytes = (size_t)hash_size * (size_t)(pl & 0x3FU);
                                        if (hash_size <= 3U && path_bytes > 0U && path_bytes <= sizeof(peer_out_path)) {
                                            memcpy(peer_out_path, c->out_path, path_bytes);
                                        } else {
                                            peer_out_path_len = -1;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        companion_unlock();
                    }

                    /* Fallback to heard adverts if peer was not yet saved in contacts */
                    if (!matched && stats_lock(pdMS_TO_TICKS(50))) {
                        for (uint8_t ai = 0; ai < s_stats.advert_count && ai < MESHCORE_MAX_ADVERTS; ai++) {
                            const meshcore_advert_t *adv = &s_node.adverts[ai];
                            if (adv->pub_key[0] == src_hash) {
                                uint8_t shared_secret[32];
                                ed25519_key_exchange(shared_secret, adv->pub_key, s_prv_key);
                                dec_len = meshcore_mac_then_decrypt(shared_secret, plain, sizeof(plain),
                                                                    &s_pkt.payload[2], s_pkt.payload_len - 2);
                                if (dec_len > 5) {
                                    matched = true;
                                    memcpy(peer_pub, adv->pub_key, sizeof(peer_pub));
                                    strncpy(peer_name, adv->name, sizeof(peer_name) - 1);
                                    peer_name[sizeof(peer_name) - 1] = '\0';
                                    peer_out_path_len = -1;
                                    break;
                                }
                            }
                        }
                        stats_unlock();
                    }

                    if (matched && dec_len > 5) {
                        uint32_t sender_ts = 0;
                        memcpy(&sender_ts, plain, 4);
                        uint8_t txt_type = plain[4] >> 2;

                        size_t str_len = (size_t)dec_len - 5;
                        size_t actual_len = 0;
                        while (actual_len < str_len && plain[5 + actual_len] != '\0') {
                            actual_len++;
                        }
                        char text[160] = {0};
                        if (actual_len >= sizeof(text)) actual_len = sizeof(text) - 1;
                        memcpy(text, &plain[5], actual_len);
                        text[actual_len] = '\0';

                        meshcore_msg_t msg = {
                            .timestamp = sender_ts,
                            .rx_time_s = (uint32_t)time(NULL),
                            .rssi_dbm = rssi,
                            .snr_q4 = snr_q4,
                            .hops = hops,
                            .outgoing = false,
                            .is_direct = true,
                            .channel_idx = 0,
                            .ack_status = MESHCORE_ACK_NONE,
                        };
                        memcpy(msg.peer_pub, peer_pub, 32);
                        snprintf(msg.sender, sizeof(msg.sender), "%s", peer_name[0] ? peer_name : "Direct");
                        snprintf(msg.body, sizeof(msg.body), "%s", text);
                        int n = snprintf(msg.text, sizeof(msg.text), "[%s] %s",
                                         peer_name[0] ? peer_name : "Direct", text);
                        if (n <= 0) {
                            strncpy(msg.text, text, sizeof(msg.text) - 1);
                        }

                        bool is_dup_msg = false;
                        bool notify_msg = false;
                        if (stats_lock(pdMS_TO_TICKS(50))) {
                            is_dup_msg = meshcore_node_is_dup_message(&s_node, &s_stats, sender_ts, msg.rx_time_s, msg.text);
                            if (!is_dup_msg) {
                                meshcore_node_append_message(&s_node, &s_stats, &msg);
                                mark_msgs_dirty_locked(now_ms);
                                notify_msg = true;
                                strncpy(s_stats.last_sender, peer_name, sizeof(s_stats.last_sender) - 1);
                                s_stats.last_sender[sizeof(s_stats.last_sender) - 1] = '\0';
                            }
                            stats_unlock();
                        }
                        if (notify_msg && s_rx_notify_cb) {
                            s_rx_notify_cb();
                        }

                        if (!is_dup_msg) {
                            if (s_msg_mutex == NULL) {
                                s_msg_mutex = xSemaphoreCreateMutex();
                            }
                            if (s_msg_mutex && xSemaphoreTake(s_msg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                                if (s_msg_count < COMPANION_MSG_QUEUE_SIZE) {
                                    companion_frame_t *mf = &s_msg_queue[s_msg_tail];
                                    mf->data[0] = RESP_CODE_CONTACT_MSG_RECV_V3;
                                    mf->data[1] = (uint8_t)snr_q4;
                                    mf->data[2] = 0;
                                    mf->data[3] = 0;
                                    memcpy(&mf->data[4], peer_pub, 6);
                                    mf->data[10] = (route == MESHCORE_ROUTE_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_FLOOD) ? hops : 0xFF;
                                    mf->data[11] = txt_type;
                                    memcpy(&mf->data[12], &sender_ts, 4);
                                    memcpy(&mf->data[16], text, actual_len);
                                    mf->len = (uint8_t)(16 + actual_len);
                                    s_msg_tail = (s_msg_tail + 1) % COMPANION_MSG_QUEUE_SIZE;
                                    s_msg_count++;
                                }
                                xSemaphoreGive(s_msg_mutex);
                            }

                            uint8_t tickle = PUSH_CODE_MSG_WAITING;
                            push_companion_event(&tickle, 1);
                            ESP_LOGI(TAG, "Direct Message from %s: '%s'", peer_name, text);
                        }

                        /* Send ACK frame */
                        uint8_t raw_ack[64];
                        size_t raw_ack_sz = meshcore_node_build_ack(plain, 5 + actual_len,
                                                                    peer_pub, peer_out_path_len, peer_out_path,
                                                                    raw_ack, sizeof(raw_ack));
                        if (raw_ack_sz > 0) {
                            vTaskDelay(pdMS_TO_TICKS(20 + (esp_random() % 40)));
                            sx1262_transmit(raw_ack, raw_ack_sz);
                        }
                    }
                }
            } else if (ptype == MESHCORE_PAYLOAD_ACK) {
                uint8_t ack_tag[4] = {0};
                uint32_t rtt_ms = 0;
                bool matched_ack = false;
                if (stats_lock(pdMS_TO_TICKS(50))) {
                    matched_ack = meshcore_node_check_ack(&s_node, &s_pkt, now_ms, ack_tag, &rtt_ms);
                    if (matched_ack) {
                        uint16_t rtt16 = (rtt_ms > UINT16_MAX) ? UINT16_MAX : (uint16_t)rtt_ms;
                        if (meshcore_node_confirm_msg_ack(&s_node, &s_stats, ack_tag, rtt16)) {
                            mark_msgs_dirty_locked(now_ms);
                        }
                    }
                    stats_unlock();
                }
                if (matched_ack) {
                    uint8_t ack_push[9];
                    ack_push[0] = PUSH_CODE_SEND_CONFIRMED;
                    memcpy(&ack_push[1], ack_tag, 4);
                    memcpy(&ack_push[5], &rtt_ms, 4);
                    push_companion_event(ack_push, sizeof(ack_push));
                    ESP_LOGI(TAG, "Direct MSG ACK confirmed (RTT %lu ms)", (unsigned long)rtt_ms);
                    if (s_rx_notify_cb) {
                        s_rx_notify_cb();
                    }
                }
            }

            /* Client repeat forwarding */
            bool repeat_en = false;
            uint8_t hash_mode = 0;
            if (stats_lock(pdMS_TO_TICKS(50))) {
                repeat_en = s_current_cfg.client_repeat;
                hash_mode = s_current_cfg.path_hash_mode;
                stats_unlock();
            }
            uint8_t repeat_raw[256];
            size_t repeat_len = 0;
            if (repeat_en &&
                meshcore_node_prepare_client_repeat(&s_pkt, s_pub_key, hash_mode,
                                                    repeat_raw, sizeof(repeat_raw), &repeat_len)) {
                /* Forward with small randomized delay */
                vTaskDelay(pdMS_TO_TICKS(20 + (esp_random() % 50)));
                esp_err_t rep_err = sx1262_transmit(repeat_raw, repeat_len);
                if (rep_err == ESP_OK && stats_lock(pdMS_TO_TICKS(50))) {
                    s_stats.forwarded_count++;
                    stats_unlock();
                }
            }
        }

        uint32_t loop_end_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        tick_ack_timeout_and_flush(loop_end_ms);

        /* If DIO1 is still high (e.g. back-to-back packet arrived), re-trigger processing */
        if (gpio_get_level(BOARD_PIN_LORA_DIO1) == 1) {
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
    }
}

static uint32_t companion_get_time(void)
{
    return (uint32_t)time(NULL);
}

static void companion_set_time(uint32_t epoch)
{
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    timekeeping_set_utc((time_t)epoch);
    ESP_LOGI(TAG, "Companion time set: %lu (RTC updated)", (unsigned long)epoch);
}

static void on_ble_subscribed(void)
{
    if (s_msg_mutex && xSemaphoreTake(s_msg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        size_t count = s_msg_count;
        xSemaphoreGive(s_msg_mutex);
        if (count > 0) {
            uint8_t tickle = PUSH_CODE_MSG_WAITING;
            meshcore_ble_send(&tickle, 1);
            ESP_LOGI(TAG, "BLE subscribed: re-sent PUSH_CODE_MSG_WAITING (%u pending)", (unsigned)count);
        }
    }
}

static void on_ble_conn_changed(bool connected)
{
    (void)connected;
    if (s_rx_notify_cb) {
        s_rx_notify_cb();
    }
}

static void fill_companion_env(companion_env_t *env)
{
    if (!env) return;
    memset(env, 0, sizeof(*env));
    if (stats_lock(pdMS_TO_TICKS(20))) {
        env->lat_e6 = s_lat_e6;
        env->lon_e6 = s_lon_e6;
        env->batt_mv = s_batt_mv;
        stats_unlock();
    } else {
        env->lat_e6 = s_lat_e6;
        env->lon_e6 = s_lon_e6;
        env->batt_mv = s_batt_mv;
    }
    uint32_t used_kb = 0, total_kb = 0;
    storage_get_usage(&used_kb, &total_kb);
    env->storage_used_kb = used_kb;
    env->storage_total_kb = total_kb;
    env->get_time_s = companion_get_time;
    env->set_time_s = companion_set_time;
    env->stats = &s_stats;
    env->send_advert = companion_send_advert;
    env->send_channel_txt = companion_send_channel_txt;
    env->send_direct_txt = companion_send_direct_txt;
    env->set_latlon = companion_set_latlon;
    env->exec_cli = companion_exec_cli;
    env->pop_msg = companion_pop_msg;
    env->on_channels_changed = on_companion_channels_changed;
    env->on_contacts_changed = on_companion_contacts_changed;
    env->on_contact_removed = on_companion_contact_removed;
    env->send_frame_cb = companion_send_frame_cb;
    memcpy(env->pub_key, s_pub_key, 32);
}

static bool companion_send_advert(bool flood)
{
    return lora_widget_send_advert(flood) == ESP_OK;
}

static bool companion_send_channel_txt(uint8_t channel_idx, const char *text)
{
    uint32_t ts = s_companion_ctx.last_tx_msg_ts;
    return lora_widget_send_channel_msg_ex(channel_idx, ts, text ? text : "") == ESP_OK;
}

static esp_err_t send_direct_msg_internal(const uint8_t pub_key[32], uint8_t txt_type, uint32_t msg_ts,
                                          const char *text, bool force_flood, uint8_t out_ack_tag[4])
{
    if (!pub_key || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    init_sync_primitives();
    init_node_keys();
    ensure_companion_loaded();
    ensure_node_msgs_loaded();

    int8_t peer_out_path_len = -1;
    uint8_t peer_out_path[COMPANION_MAX_PATH_SIZE] = {0};
    char peer_name[COMPANION_NAME_SIZE] = {0};
    if (companion_lock(pdMS_TO_TICKS(50))) {
        const companion_contact_t *c = meshcore_companion_get_contact(&s_companion_ctx, pub_key);
        if (c) {
            if (!force_flood) {
                peer_out_path_len = c->out_path_len;
                if (peer_out_path_len != -1) {
                    uint8_t pl = (uint8_t)peer_out_path_len;
                    uint8_t hash_size = (uint8_t)((pl >> 6) + 1U);
                    size_t path_bytes = (size_t)hash_size * (size_t)(pl & 0x3FU);
                    if (hash_size <= 3U && path_bytes > 0U && path_bytes <= sizeof(peer_out_path)) {
                        memcpy(peer_out_path, c->out_path, path_bytes);
                    } else {
                        peer_out_path_len = -1;
                    }
                }
            }
            strncpy(peer_name, c->name, sizeof(peer_name) - 1);
            peer_name[sizeof(peer_name) - 1] = '\0';
        }
        companion_unlock();
    }

    bool ready = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        ready = s_stats.enabled && s_stats.initialized;
        stats_unlock();
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t msg_time = (msg_ts != 0U) ? msg_ts : (uint32_t)time(NULL);
    uint8_t attempt = force_flood ? 1U : 0U;
    bool route_direct = (!force_flood && peer_out_path_len != -1 && ((uint8_t)peer_out_path_len & 0x3FU) > 0U);
    uint8_t ack_tag[4] = {0};
    uint8_t raw[256];
    size_t raw_sz = meshcore_node_build_direct_txt(
        s_pub_key, s_prv_key, pub_key,
        peer_out_path_len, peer_out_path,
        txt_type, attempt, msg_time, text,
        ack_tag, raw, sizeof(raw));
    if (raw_sz == 0U) {
        return ESP_ERR_NO_MEM;
    }
    if (out_ack_tag) {
        memcpy(out_ack_tag, ack_tag, 4);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    meshcore_packet_t tx_pkt;
    bool decoded = meshcore_packet_decode(&tx_pkt, raw, raw_sz);
    if (stats_lock(pdMS_TO_TICKS(50))) {
        if (decoded) {
            meshcore_node_mark_seen(&s_node, &tx_pkt, now_ms);
        }
        meshcore_node_expect_ack(&s_node, ack_tag, now_ms);
        stats_unlock();
    }

    esp_err_t tx_err = enqueue_tx_packet(raw, raw_sz);
    if (tx_err != ESP_OK) {
        if (stats_lock(pdMS_TO_TICKS(50))) {
            meshcore_node_cancel_ack(&s_node, ack_tag);
            stats_unlock();
        }
        return tx_err;
    }

    meshcore_msg_t msg = {
        .timestamp = msg_time,
        .rx_time_s = (uint32_t)time(NULL),
        .rssi_dbm = 0,
        .snr_q4 = 0,
        .hops = 0,
        .outgoing = true,
        .is_direct = true,
        .channel_idx = 0,
        .ack_status = MESHCORE_ACK_PENDING,
        .route_direct = route_direct,
    };
    memcpy(msg.peer_pub, pub_key, 32);
    memcpy(msg.ack_tag, ack_tag, 4);
    snprintf(msg.sender, sizeof(msg.sender), "%s", peer_name[0] ? peer_name : "Direct");
    snprintf(msg.body, sizeof(msg.body), "%s", text);
    int n = snprintf(msg.text, sizeof(msg.text), "[->%s] %s",
                     peer_name[0] ? peer_name : "Direct", text);
    if (n <= 0) {
        strncpy(msg.text, text, sizeof(msg.text) - 1);
        msg.text[sizeof(msg.text) - 1] = '\0';
    }

    if (stats_lock(pdMS_TO_TICKS(50))) {
        s_stats.last_packet_time_s = (uint32_t)time(NULL);
        strncpy(s_stats.last_sender, peer_name[0] ? peer_name : "Direct", sizeof(s_stats.last_sender) - 1);
        s_stats.last_sender[sizeof(s_stats.last_sender) - 1] = '\0';
        meshcore_node_append_message(&s_node, &s_stats, &msg);
        mark_msgs_dirty_locked(now_ms);
        stats_unlock();
    }
    if (s_rx_notify_cb) {
        s_rx_notify_cb();
    }
    return ESP_OK;
}

static bool companion_send_direct_txt(const uint8_t pub_key[32], uint8_t txt_type, uint32_t msg_ts,
                                      const char *text, uint8_t out_ack_tag[4])
{
    return send_direct_msg_internal(pub_key, txt_type, msg_ts, text, false, out_ack_tag) == ESP_OK;
}

static void companion_set_latlon(int32_t lat_e6, int32_t lon_e6, void *user_data)
{
    (void)user_data;
    if (stats_lock(pdMS_TO_TICKS(20))) {
        s_lat_e6 = lat_e6;
        s_lon_e6 = lon_e6;
        s_has_loc = (lat_e6 != 0 || lon_e6 != 0);
        stats_unlock();
    } else {
        s_lat_e6 = lat_e6;
        s_lon_e6 = lon_e6;
        s_has_loc = (lat_e6 != 0 || lon_e6 != 0);
    }
}

static bool s_companion_cli_cfg_changed = false;
static lora_config_t *s_companion_active_cfg = NULL;

static void companion_exec_cli(const char *cmd, char *reply, size_t reply_sz)
{
    bool changed = false;
    lora_config_t *target_cfg = s_companion_active_cfg ? s_companion_active_cfg : &s_current_cfg;
    lora_widget_exec_cli(cmd, target_cfg, &changed, reply, reply_sz);
    if (changed) {
        s_companion_cli_cfg_changed = true;
    }
}

static void companion_send_frame_cb(const uint8_t *frame, size_t len, void *user_data)
{
    (void)user_data;
    meshcore_ble_send(frame, len);
}

static void on_ble_rx(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > sizeof(s_ble_cmd_queue[0].data)) return;
    if (s_ble_cmd_mutex == NULL) {
        s_ble_cmd_mutex = xSemaphoreCreateMutex();
    }
    if (s_ble_cmd_mutex && xSemaphoreTake(s_ble_cmd_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_ble_cmd_count < BLE_CMD_QUEUE_SIZE) {
            s_ble_cmd_queue[s_ble_cmd_tail].len = (uint16_t)len;
            memcpy(s_ble_cmd_queue[s_ble_cmd_tail].data, data, len);
            s_ble_cmd_tail = (s_ble_cmd_tail + 1) % BLE_CMD_QUEUE_SIZE;
            s_ble_cmd_count++;
        }
        xSemaphoreGive(s_ble_cmd_mutex);
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void push_companion_event(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > COMPANION_MAX_FRAME_SIZE) return;
    if (s_push_mutex == NULL) {
        s_push_mutex = xSemaphoreCreateMutex();
    }
    if (s_push_mutex && xSemaphoreTake(s_push_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_push_count < COMPANION_PUSH_QUEUE_SIZE) {
            s_push_queue[s_push_tail].len = (uint8_t)len;
            memcpy(s_push_queue[s_push_tail].data, data, len);
            s_push_tail = (s_push_tail + 1) % COMPANION_PUSH_QUEUE_SIZE;
            s_push_count++;
        }
        xSemaphoreGive(s_push_mutex);
    }
    meshcore_ble_send(data, len);
}

static bool companion_pop_msg(uint8_t *msg_out, size_t max, size_t *out_len)
{
    if (!msg_out || max == 0 || !out_len) return false;
    bool popped = false;
    if (s_msg_mutex == NULL) {
        s_msg_mutex = xSemaphoreCreateMutex();
    }
    if (s_msg_mutex && xSemaphoreTake(s_msg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_msg_count > 0) {
            size_t len = s_msg_queue[s_msg_head].len;
            if (len > max) len = max;
            memcpy(msg_out, s_msg_queue[s_msg_head].data, len);
            *out_len = len;
            s_msg_head = (s_msg_head + 1) % COMPANION_MSG_QUEUE_SIZE;
            s_msg_count--;
            popped = true;
        }
        xSemaphoreGive(s_msg_mutex);
    }
    return popped;
}

size_t lora_widget_handle_companion_cmd(const uint8_t *cmd, size_t cmd_len,
                                        lora_config_t *cfg_inout, bool *cfg_changed,
                                        uint8_t *resp_out, size_t resp_max)
{
    if (!cmd || cmd_len == 0 || !resp_out || resp_max == 0) {
        return 0;
    }
    init_sync_primitives();
    if (!companion_lock(pdMS_TO_TICKS(200))) {
        return 0;
    }
    if (!s_companion_initialized) {
        meshcore_companion_init(&s_companion_ctx);
        load_channels_from_storage(&s_companion_ctx);
        load_contacts_from_storage(&s_companion_ctx);
        s_companion_initialized = true;
    }

    companion_env_t env;
    fill_companion_env(&env);

    lora_config_t *target_cfg = cfg_inout ? cfg_inout : &s_current_cfg;
    s_companion_cli_cfg_changed = false;
    s_companion_active_cfg = target_cfg;
    size_t resp_len = meshcore_companion_handle_cmd(&s_companion_ctx, &env,
                                                    target_cfg,
                                                    cfg_changed,
                                                    cmd, cmd_len, resp_out, resp_max);
    s_companion_active_cfg = NULL;
    if (s_companion_cli_cfg_changed && cfg_changed) {
        *cfg_changed = true;
    }
    companion_unlock();
    return resp_len;
}

bool lora_widget_pop_companion_push(uint8_t *push_out, size_t push_max, size_t *push_len)
{
    if (!push_out || push_max == 0 || !push_len) return false;
    bool popped = false;
    if (s_push_mutex == NULL) {
        s_push_mutex = xSemaphoreCreateMutex();
    }
    if (s_push_mutex && xSemaphoreTake(s_push_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_push_count > 0) {
            size_t len = s_push_queue[s_push_head].len;
            if (len > push_max) len = push_max;
            memcpy(push_out, s_push_queue[s_push_head].data, len);
            *push_len = len;
            s_push_head = (s_push_head + 1) % COMPANION_PUSH_QUEUE_SIZE;
            s_push_count--;
            popped = true;
        }
        xSemaphoreGive(s_push_mutex);
    }
    return popped;
}

static bool is_rf_config_changed(const lora_config_t *a, const lora_config_t *b)
{
    return (a->enabled != b->enabled ||
            a->freq_khz != b->freq_khz ||
            a->bw_hz != b->bw_hz ||
            a->bw_khz != b->bw_khz ||
            a->sf != b->sf ||
            a->cr != b->cr ||
            a->tx_power_dbm != b->tx_power_dbm ||
            a->sync_word != b->sync_word ||
            a->rx_boosted != b->rx_boosted);
}

static void normalize_cfg(const lora_config_t *cfg, lora_config_t *out_local, uint32_t *out_pin)
{
    lora_config_t local = *cfg;
    if (local.node_name[0] == '\0') {
        strncpy(local.node_name, "NM-EPD-420", sizeof(local.node_name) - 1);
        local.node_name[sizeof(local.node_name) - 1] = '\0';
    }
    if (local.channel_psk[0] == '\0') {
        strncpy(local.channel_psk, LORA_DEFAULT_PUBLIC_PSK, sizeof(local.channel_psk) - 1);
        local.channel_psk[sizeof(local.channel_psk) - 1] = '\0';
    }
    *out_local = local;
    *out_pin = (local.ble_pin >= 100000 && local.ble_pin <= 999999)
                   ? local.ble_pin
                   : get_default_ble_pin();
}

static void sync_meta_locked(const lora_config_t *cfg_local, uint32_t active_pin,
                             bool has_ch_hash, uint8_t ch_hash)
{
    s_current_cfg = *cfg_local;
    if (has_ch_hash) {
        s_stats.channel_hash = ch_hash;
    }
    s_stats.freq_khz = cfg_local->freq_khz;
    s_stats.bw_hz = cfg_local->bw_hz;
    s_stats.bw_khz = cfg_local->bw_khz;
    s_stats.sf = cfg_local->sf;
    s_stats.cr = cfg_local->cr;
    s_stats.tx_power_dbm = cfg_local->tx_power_dbm;
    s_stats.sync_word = cfg_local->sync_word;
    s_stats.rx_boosted = cfg_local->rx_boosted;
    s_stats.cad_enabled = cfg_local->cad_enabled;
    s_stats.client_repeat = cfg_local->client_repeat;
    s_stats.path_hash_mode = cfg_local->path_hash_mode;
    s_stats.airtime_factor_x10 = cfg_local->airtime_factor_x10;
    s_stats.wake_on_rx = cfg_local->wake_on_rx;
    s_stats.share_loc = cfg_local->share_loc;
    s_stats.ble_pin = active_pin;
    strncpy(s_stats.node_name, s_current_cfg.node_name, sizeof(s_stats.node_name) - 1);
    s_stats.node_name[sizeof(s_stats.node_name) - 1] = '\0';
    strncpy(s_stats.channel_name, s_current_cfg.channel_name, sizeof(s_stats.channel_name) - 1);
    s_stats.channel_name[sizeof(s_stats.channel_name) - 1] = '\0';
}

void lora_widget_sync_config(const lora_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    init_sync_primitives();
    init_node_keys();
    ensure_companion_loaded();
    ensure_node_msgs_loaded();

    lora_config_t cfg_local;
    uint32_t active_pin = 0;
    normalize_cfg(cfg, &cfg_local, &active_pin);
    sync_slot0_channel_from_cfg(&cfg_local);

    uint8_t secret32[32];
    uint8_t ch_hash = 0;
    bool has_ch_hash = meshcore_channel_key_from_psk(cfg_local.channel_psk, secret32, &ch_hash);

    if (stats_lock(pdMS_TO_TICKS(50))) {
        sync_meta_locked(&cfg_local, active_pin, has_ch_hash, ch_hash);
        stats_unlock();
    }
}

esp_err_t lora_widget_apply(const lora_config_t *cfg)
{
    init_sync_primitives();
    init_node_keys();
    ensure_companion_loaded();
    ensure_node_msgs_loaded();

    lora_config_t cfg_local = { 0 };
    uint32_t active_pin = get_default_ble_pin();
    bool has_cfg = (cfg != NULL);
    if (has_cfg) {
        normalize_cfg(cfg, &cfg_local, &active_pin);
        sync_slot0_channel_from_cfg(&cfg_local);
    }
    if (stats_lock(pdMS_TO_TICKS(50))) {
        s_stats.ble_pin = active_pin;
        stats_unlock();
    }

    if (!has_cfg || !cfg_local.enabled) {
        meshcore_ble_stop();
        flush_dirty_contacts_if_due(true, 0);
        flush_dirty_msgs_if_due(true, 0);

        if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, pdMS_TO_TICKS(1500)) == pdTRUE) {
            bool was_init = false;
            if (stats_lock(pdMS_TO_TICKS(50))) {
                was_init = s_stats.initialized;
                stats_unlock();
            }
            sx1262_radio_power_off(s_task, was_init);
            xSemaphoreGive(s_radio_mutex);
        }

        if (stats_lock(pdMS_TO_TICKS(50))) {
            s_ble_paused_for_sleep = false;
            s_stats.enabled = false;
            s_stats.initialized = false;
            s_stats.ble_pin = active_pin;
            if (has_cfg) {
                s_current_cfg.enabled = false;
            }
            stats_unlock();
        }
        ESP_LOGI(TAG, "LoRa radio disabled");
        return ESP_OK;
    }

    /* Fast path: if radio is already initialized and RF parameters didn't change, skip SX1262 HW reset */
    bool fast_path = false;
    uint8_t secret32[32];
    uint8_t ch_hash = 0;
    bool has_ch_hash = meshcore_channel_key_from_psk(cfg_local.channel_psk, secret32, &ch_hash);

    if (stats_lock(pdMS_TO_TICKS(50))) {
        if (s_stats.initialized && !is_rf_config_changed(&s_current_cfg, &cfg_local)) {
            fast_path = true;
            s_ble_paused_for_sleep = false;
            s_current_cfg = cfg_local;
            if (has_ch_hash) {
                s_stats.channel_hash = ch_hash;
            }
            s_stats.cad_enabled = cfg_local.cad_enabled;
            s_stats.client_repeat = cfg_local.client_repeat;
            s_stats.path_hash_mode = cfg_local.path_hash_mode;
            s_stats.airtime_factor_x10 = cfg_local.airtime_factor_x10;
            s_stats.wake_on_rx = cfg_local.wake_on_rx;
            s_stats.share_loc = cfg_local.share_loc;
            s_stats.ble_pin = active_pin;
            strncpy(s_stats.node_name, s_current_cfg.node_name, sizeof(s_stats.node_name) - 1);
            s_stats.node_name[sizeof(s_stats.node_name) - 1] = '\0';
            strncpy(s_stats.channel_name, s_current_cfg.channel_name, sizeof(s_stats.channel_name) - 1);
            s_stats.channel_name[sizeof(s_stats.channel_name) - 1] = '\0';
        }
        stats_unlock();
    }

    if (fast_path) {
        meshcore_ble_init(cfg_local.node_name, active_pin, on_ble_rx);
        meshcore_ble_set_subscribe_cb(on_ble_subscribed);
        meshcore_ble_set_conn_cb(on_ble_conn_changed);
        if (s_task != NULL) {
            xTaskNotifyGive(s_task);
        }
        return ESP_OK;
    }

    /* Full hardware reconfiguration: commit config and mark uninitialized before locking radio */
    if (stats_lock(pdMS_TO_TICKS(50))) {
        s_ble_paused_for_sleep = false;
        s_current_cfg = cfg_local;
        if (has_ch_hash) {
            s_stats.channel_hash = ch_hash;
        }
        s_stats.initialized = false;
        stats_unlock();
    }

    /* Ensure background receiver task exists before arming DIO1 ISR */
    if (s_task == NULL) {
        xTaskCreate(lora_rx_task, "lora_rx", 16384, NULL, 5, &s_task);
    }

    if (!s_radio_mutex || xSemaphoreTake(s_radio_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t bw_hz = 250000;
    uint8_t sf = 7;
    uint8_t cr = 5;
    int8_t pwr = 22;
    uint16_t sw = 0x1424;
    uint8_t ldro = 0;
    bool hw_ok = false;

    esp_err_t rc = sx1262_radio_configure(&cfg_local, s_task,
                                          &bw_hz, &sf, &cr, &pwr, &sw, &ldro, &hw_ok);
    xSemaphoreGive(s_radio_mutex);

    if (rc != ESP_OK) {
        return rc;
    }

    if (!hw_ok) {
        if (stats_lock(pdMS_TO_TICKS(50))) {
            s_stats.enabled = true;
            s_stats.initialized = false;
            stats_unlock();
        }
        return ESP_OK;
    }

    if (stats_lock(pdMS_TO_TICKS(50))) {
        s_stats.initialized = true;
        s_stats.enabled = true;
        s_stats.freq_khz = cfg_local.freq_khz;
        s_stats.bw_hz = bw_hz;
        s_stats.bw_khz = (uint16_t)(bw_hz / 1000U);
        s_stats.sf = sf;
        s_stats.cr = cr;
        s_stats.tx_power_dbm = pwr;
        s_stats.sync_word = sw;
        s_stats.rx_boosted = cfg_local.rx_boosted;
        s_stats.cad_enabled = cfg_local.cad_enabled;
        s_stats.client_repeat = cfg_local.client_repeat;
        s_stats.path_hash_mode = cfg_local.path_hash_mode;
        s_stats.airtime_factor_x10 = cfg_local.airtime_factor_x10;
        s_stats.wake_on_rx = cfg_local.wake_on_rx;
        s_stats.share_loc = cfg_local.share_loc;
        s_stats.ble_pin = active_pin;
        strncpy(s_stats.node_name, s_current_cfg.node_name, sizeof(s_stats.node_name) - 1);
        s_stats.node_name[sizeof(s_stats.node_name) - 1] = '\0';
        strncpy(s_stats.channel_name, s_current_cfg.channel_name, sizeof(s_stats.channel_name) - 1);
        s_stats.channel_name[sizeof(s_stats.channel_name) - 1] = '\0';
        stats_unlock();
    }

    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }

    meshcore_ble_init(cfg_local.node_name, active_pin, on_ble_rx);
    meshcore_ble_set_subscribe_cb(on_ble_subscribed);
    meshcore_ble_set_conn_cb(on_ble_conn_changed);

    ESP_LOGI(TAG, "SX1262 MeshCore configured: %lu kHz, SF%u, BW %lu Hz, CR 4/%u, Pwr %d dBm, Sync 0x%04X, LDRO %u",
             (unsigned long)cfg_local.freq_khz, (unsigned)sf, (unsigned long)bw_hz,
             (unsigned)cr, (int)pwr, (unsigned)sw, (unsigned)ldro);
    return ESP_OK;
}

esp_err_t lora_widget_init(void)
{
    init_sync_primitives();
    ensure_companion_loaded();
    ensure_node_msgs_loaded();
    lora_config_t def = {
        .enabled = true,
        .freq_khz = 868000,
        .bw_hz = 250000,
        .bw_khz = 250,
        .sf = 7,
        .cr = 5,
        .tx_power_dbm = 22,
        .sync_word = 0x1424,
        .rx_boosted = true,
        .cad_enabled = false,
        .client_repeat = false,
        .path_hash_mode = 0,
        .airtime_factor_x10 = 10,
        .wake_on_rx = false,
        .share_loc = false,
        .ble_pin = 0,
        .node_name = "NM-EPD-420",
        .channel_name = "Public",
        .channel_psk = LORA_DEFAULT_PUBLIC_PSK,
    };
    return lora_widget_apply(&def);
}

void lora_widget_sleep(void)
{
    meshcore_ble_stop();
    flush_dirty_contacts_if_due(true, 0);
    flush_dirty_msgs_if_due(true, 0);

    bool wake_keep = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        s_ble_paused_for_sleep = false;
        wake_keep = s_stats.enabled && s_stats.wake_on_rx;
        stats_unlock();
    }
    if (wake_keep) {
        return;
    }

    if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        bool was_init = false;
        if (stats_lock(pdMS_TO_TICKS(50))) {
            was_init = s_stats.initialized;
            stats_unlock();
        }
        sx1262_radio_power_off(s_task, was_init);
        xSemaphoreGive(s_radio_mutex);
    }

    if (stats_lock(pdMS_TO_TICKS(50))) {
        s_stats.enabled = false;
        s_stats.initialized = false;
        stats_unlock();
    }
}

void lora_widget_notify(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void lora_widget_reset_stats(void)
{
    if (stats_lock(pdMS_TO_TICKS(50))) {
        meshcore_node_reset(&s_node, &s_stats);
        s_stats.forwarded_count = 0;
        stats_unlock();
    }
}

lora_stats_t lora_widget_get_stats(void)
{
    lora_stats_t copy = {0};
    if (stats_lock(pdMS_TO_TICKS(20))) {
        s_stats.ble_connected = meshcore_ble_is_connected();
        copy = s_stats;
        stats_unlock();
    }
    return copy;
}

void lora_widget_set_location(int32_t lat_e4, int32_t lon_e4)
{
    if (stats_lock(pdMS_TO_TICKS(20))) {
        s_lat_e6 = lat_e4 * 100;
        s_lon_e6 = lon_e4 * 100;
        s_has_loc = (lat_e4 != 0 || lon_e4 != 0);
        stats_unlock();
    } else {
        s_lat_e6 = lat_e4 * 100;
        s_lon_e6 = lon_e4 * 100;
        s_has_loc = (lat_e4 != 0 || lon_e4 != 0);
    }
}

void lora_widget_set_battery(uint16_t batt_mv, uint8_t level_pct)
{
    uint8_t pct = (level_pct > 100) ? 100 : level_pct;
    uint16_t mv = (batt_mv >= 2500 && batt_mv <= 4500)
                      ? batt_mv
                      : (uint16_t)(3000 + (uint16_t)pct * 12);
    if (stats_lock(pdMS_TO_TICKS(20))) {
        s_batt_pct = pct;
        s_batt_mv = mv;
        stats_unlock();
    } else {
        s_batt_pct = pct;
        s_batt_mv = mv;
    }
}

esp_err_t lora_widget_send_advert(bool flood)
{
    init_sync_primitives();
    init_node_keys();

    bool ready = false;
    char node_name[32] = {0};
    bool send_loc = false;
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        ready = s_stats.enabled && s_stats.initialized;
        strncpy(node_name, s_current_cfg.node_name, sizeof(node_name) - 1);
        send_loc = s_current_cfg.share_loc && s_has_loc;
        lat_e6 = s_lat_e6;
        lon_e6 = s_lon_e6;
        stats_unlock();
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[256];
    size_t raw_sz = meshcore_build_advert(
        raw, sizeof(raw),
        flood,
        s_pub_key, s_prv_key,
        (uint32_t)time(NULL),
        node_name,
        MESHCORE_ADV_CHAT,
        send_loc,
        lat_e6, lon_e6
    );

    if (raw_sz == 0) {
        return ESP_ERR_NO_MEM;
    }

    meshcore_packet_t tx_pkt;
    if (meshcore_packet_decode(&tx_pkt, raw, raw_sz)) {
#ifdef ESP_PLATFORM
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
#else
        uint32_t now_ms = (uint32_t)(time(NULL) * 1000ULL);
#endif
        if (stats_lock(pdMS_TO_TICKS(50))) {
            meshcore_node_mark_seen(&s_node, &tx_pkt, now_ms);
            stats_unlock();
        }
    }

    esp_err_t err = enqueue_tx_packet(raw, raw_sz);
    return err;
}

esp_err_t lora_widget_send_discover(void)
{
    init_sync_primitives();

    bool ready = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        ready = s_stats.enabled && s_stats.initialized;
        stats_unlock();
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t tag = esp_random();
    uint8_t raw[64];
    size_t raw_sz = meshcore_build_discover_req(raw, sizeof(raw), tag);
    if (raw_sz == 0) {
        return ESP_ERR_NO_MEM;
    }

    return enqueue_tx_packet(raw, raw_sz);
}

esp_err_t lora_widget_send_channel_msg_ex(uint8_t channel_idx, uint32_t ts, const char *text)
{
    if (!text || text[0] == '\0' || channel_idx >= COMPANION_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    init_sync_primitives();
    ensure_companion_loaded();
    ensure_node_msgs_loaded();

    uint8_t secret[16] = {0};
    bool have_secret = false;
    char ch_name[COMPANION_NAME_SIZE] = {0};

    if (companion_lock(pdMS_TO_TICKS(50))) {
        if (channel_idx < COMPANION_MAX_CHANNELS && s_companion_ctx.channels[channel_idx].valid) {
            memcpy(secret, s_companion_ctx.channels[channel_idx].secret, 16);
            strncpy(ch_name, s_companion_ctx.channels[channel_idx].name, sizeof(ch_name) - 1);
            ch_name[sizeof(ch_name) - 1] = '\0';
            have_secret = true;
        }
        companion_unlock();
    }

    bool ready = false;
    char node_name[32] = {0};
    char psk_copy[64] = {0};
    if (stats_lock(pdMS_TO_TICKS(50))) {
        ready = s_stats.enabled && s_stats.initialized;
        strncpy(node_name, s_current_cfg.node_name, sizeof(node_name) - 1);
        strncpy(psk_copy, s_current_cfg.channel_psk, sizeof(psk_copy) - 1);
        stats_unlock();
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[256];
    uint32_t msg_time = (ts != 0) ? ts : (uint32_t)time(NULL);
    size_t raw_sz = 0;

    if (have_secret) {
        raw_sz = meshcore_build_grp_txt_secret(
            raw, sizeof(raw),
            secret,
            msg_time,
            node_name,
            text
        );
    } else {
        raw_sz = meshcore_build_grp_txt(
            raw, sizeof(raw),
            psk_copy,
            msg_time,
            node_name,
            text
        );
    }

    if (raw_sz == 0) {
        return ESP_ERR_NO_MEM;
    }

    meshcore_packet_t tx_pkt;
    if (meshcore_packet_decode(&tx_pkt, raw, raw_sz)) {
#ifdef ESP_PLATFORM
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
#else
        uint32_t now_ms = (uint32_t)(time(NULL) * 1000ULL);
#endif
        if (stats_lock(pdMS_TO_TICKS(50))) {
            meshcore_node_mark_seen(&s_node, &tx_pkt, now_ms);
            stats_unlock();
        }
    }

    esp_err_t err = enqueue_tx_packet(raw, raw_sz);
    if (err == ESP_OK) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        meshcore_msg_t msg = {
            .timestamp = msg_time,
            .rx_time_s = (uint32_t)time(NULL),
            .rssi_dbm = 0,
            .snr_q4 = 0,
            .hops = 0,
            .outgoing = true,
            .is_direct = false,
            .channel_idx = channel_idx,
            .ack_status = MESHCORE_ACK_NONE,
        };
        snprintf(msg.sender, sizeof(msg.sender), "%s", node_name);
        snprintf(msg.body, sizeof(msg.body), "%s", text);
        if (ch_name[0] != '\0') {
            int n = snprintf(msg.text, sizeof(msg.text), "[%s] %s: ", ch_name, node_name);
            if (n > 0 && (size_t)n < sizeof(msg.text)) {
                strncat(msg.text, text, sizeof(msg.text) - 1 - (size_t)n);
            }
        } else {
            int n = snprintf(msg.text, sizeof(msg.text), "%s: ", node_name);
            if (n > 0 && (size_t)n < sizeof(msg.text)) {
                strncat(msg.text, text, sizeof(msg.text) - 1 - (size_t)n);
            }
        }

        if (stats_lock(pdMS_TO_TICKS(50))) {
            s_stats.last_packet_time_s = (uint32_t)time(NULL);
            strncpy(s_stats.last_sender, node_name, sizeof(s_stats.last_sender) - 1);
            s_stats.last_sender[sizeof(s_stats.last_sender) - 1] = '\0';
            meshcore_node_append_message(&s_node, &s_stats, &msg);
            mark_msgs_dirty_locked(now_ms);
            stats_unlock();
        }
        if (s_rx_notify_cb) {
            s_rx_notify_cb();
        }
    }
    return err;
}

esp_err_t lora_widget_send_channel_msg_ts(uint32_t ts, const char *text)
{
    return lora_widget_send_channel_msg_ex(0, ts, text);
}

esp_err_t lora_widget_send_channel_msg(const char *text)
{
    return lora_widget_send_channel_msg_ts(0, text);
}

typedef struct {
    bool ble_name_changed;
    char new_ble_name[LORA_NODE_NAME_LEN];
    bool ble_pin_changed;
    uint32_t new_ble_pin;
} lora_cli_ctx_t;

static void cli_reset_stats_cb(void *ctx)
{
    (void)ctx;
    lora_widget_reset_stats();
}

static bool cli_send_advert_cb(void *ctx, bool flood)
{
    (void)ctx;
    return lora_widget_send_advert(flood) == ESP_OK;
}

static bool cli_send_discover_cb(void *ctx)
{
    (void)ctx;
    return lora_widget_send_discover() == ESP_OK;
}

static bool cli_send_channel_msg_cb(void *ctx, uint8_t channel_idx, const char *text)
{
    (void)ctx;
    return lora_widget_send_channel_msg_ex(channel_idx, 0, text) == ESP_OK;
}

static void cli_update_ble_name_cb(void *ctx, const char *node_name)
{
    lora_cli_ctx_t *c = (lora_cli_ctx_t *)ctx;
    if (!c || !node_name) return;
    strncpy(c->new_ble_name, node_name, sizeof(c->new_ble_name) - 1);
    c->new_ble_name[sizeof(c->new_ble_name) - 1] = '\0';
    c->ble_name_changed = true;
}

static void cli_update_ble_pin_cb(void *ctx, uint32_t pin)
{
    lora_cli_ctx_t *c = (lora_cli_ctx_t *)ctx;
    if (!c) return;
    c->new_ble_pin = pin;
    c->ble_pin_changed = true;
}

static size_t utf8_safe_len(const char *s, size_t max_bytes)
{
    size_t len = strlen(s);
    if (len <= max_bytes) {
        return len;
    }
    len = max_bytes;
    while (len > 0 && (((unsigned char)s[len] & 0xC0U) == 0x80U)) {
        len--;
    }
    return len;
}

static void cli_list_contacts_cb(void *ctx, char *reply, size_t reply_sz)
{
    (void)ctx;
    if (!reply || reply_sz == 0) return;
    reply[0] = '\0';
    ensure_companion_loaded();
    size_t pos = 0;
    if (companion_lock(pdMS_TO_TICKS(100))) {
        size_t cnt = s_companion_ctx.contact_count;
        int hdr = snprintf(reply, reply_sz, "Contacts (%u):\n", (unsigned)cnt);
        if (hdr > 0) {
            pos = (size_t)hdr < reply_sz ? (size_t)hdr : reply_sz - 1;
        }
        for (size_t i = 0; i < cnt; i++) {
            const companion_contact_t *c = &s_companion_ctx.contacts[i];
            int hops = (c->out_path_len != -1) ? ((uint8_t)c->out_path_len & 0x3F) : -1;
            char line[128];
            int ln = snprintf(line, sizeof(line),
                              "  [%u] %s (%02x%02x%02x%02x) hops=%d\n",
                              (unsigned)i,
                              c->name[0] ? c->name : "(unnamed)",
                              c->pub_key[0], c->pub_key[1], c->pub_key[2], c->pub_key[3],
                              hops);
            if (ln <= 0) continue;
            size_t line_len = (size_t)ln < sizeof(line) ? (size_t)ln : sizeof(line) - 1;
            size_t remaining_items = cnt - i - 1;
            size_t footer_reserve = (remaining_items > 0) ? 28U : 1U;
            if (pos + line_len + footer_reserve > reply_sz) {
                if (pos + 28U <= reply_sz) {
                    int fn = snprintf(reply + pos, reply_sz - pos,
                                      "  ... (%u more)\n", (unsigned)(cnt - i));
                    if (fn > 0) pos += (size_t)fn;
                } else if (reply_sz > pos + 1U) {
                    size_t fit = utf8_safe_len(line, reply_sz - pos - 1U);
                    memcpy(reply + pos, line, fit);
                    pos += fit;
                    reply[pos] = '\0';
                }
                break;
            }
            memcpy(reply + pos, line, line_len);
            pos += line_len;
            reply[pos] = '\0';
        }
        companion_unlock();
    } else {
        snprintf(reply, reply_sz, "ERR busy\n");
    }
}

static void cli_list_channels_cb(void *ctx, char *reply, size_t reply_sz)
{
    (void)ctx;
    ensure_companion_loaded();
    size_t pos = 0;
    if (companion_lock(pdMS_TO_TICKS(100))) {
        pos += (size_t)snprintf(reply + pos, reply_sz - pos, "Channels:\n");
        for (size_t i = 0; i < COMPANION_MAX_CHANNELS && pos + 1 < reply_sz; i++) {
            const companion_channel_t *ch = &s_companion_ctx.channels[i];
            if (ch->valid) {
                pos += (size_t)snprintf(reply + pos, reply_sz - pos,
                                        "  [%u] %s\n", (unsigned)i, ch->name);
            } else {
                pos += (size_t)snprintf(reply + pos, reply_sz - pos,
                                        "  [%u] (empty)\n", (unsigned)i);
            }
            if (pos >= reply_sz) {
                pos = reply_sz - 1;
                break;
            }
        }
        companion_unlock();
    } else {
        snprintf(reply, reply_sz, "ERR busy\n");
    }
}

void lora_widget_exec_cli(const char *cmd, lora_config_t *cfg_inout, bool *cfg_changed, char *reply, size_t reply_sz)
{
    if (!reply || reply_sz == 0) {
        return;
    }
    reply[0] = '\0';
    if (cfg_changed) *cfg_changed = false;

    const char *trimmed = cmd ? cmd : "";
    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
    if (s_rekey_cb != NULL && strcasecmp(trimmed, "reset-identity") == 0) {
        bool ok = s_rekey_cb();
        if (stats_lock(pdMS_TO_TICKS(50))) {
            snprintf(reply, reply_sz, "Identity reset: %s (PubKey: %s, PIN: %06lu)\n",
                     ok ? "OK" : "FAIL", s_stats.pub_key_hex, (unsigned long)s_stats.ble_pin);
            stats_unlock();
        } else {
            snprintf(reply, reply_sz, "Identity reset: %s\n", ok ? "OK" : "FAIL");
        }
        return;
    }

    /* Handle contacts/channels outside s_stats_mutex to preserve lock order (s_companion_mutex -> s_stats_mutex) */
    if (strcasecmp(trimmed, "contacts") == 0) {
        cli_list_contacts_cb(NULL, reply, reply_sz);
        return;
    }
    if (strcasecmp(trimmed, "channels") == 0) {
        cli_list_channels_cb(NULL, reply, reply_sz);
        return;
    }

    char ch_names[COMPANION_MAX_CHANNELS][LORA_CHANNEL_NAME_LEN] = {{0}};
    ensure_companion_loaded();
    if (s_companion_mutex && xSemaphoreTake(s_companion_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < COMPANION_MAX_CHANNELS; i++) {
            if (s_companion_ctx.channels[i].valid) {
                strncpy(ch_names[i], s_companion_ctx.channels[i].name, LORA_CHANNEL_NAME_LEN - 1);
                ch_names[i][LORA_CHANNEL_NAME_LEN - 1] = '\0';
            }
        }
        xSemaphoreGive(s_companion_mutex);
    }

    lora_cli_ctx_t cli_ctx = {0};
    meshcore_cli_env_t env = {
        .reset_stats = cli_reset_stats_cb,
        .send_advert = cli_send_advert_cb,
        .send_discover = cli_send_discover_cb,
        .send_channel_msg = cli_send_channel_msg_cb,
        .update_ble_name = cli_update_ble_name_cb,
        .update_ble_pin = cli_update_ble_pin_cb,
        .list_contacts = cli_list_contacts_cb,
        .list_channels = cli_list_channels_cb,
        .channel_names = ch_names,
        .channel_names_count = COMPANION_MAX_CHANNELS,
        .user_data = &cli_ctx,
    };

    if (stats_lock(pdMS_TO_TICKS(50))) {
        s_stats.ble_connected = meshcore_ble_is_connected();
        if (cfg_inout != NULL && s_current_cfg.freq_khz == 0) {
            s_current_cfg = *cfg_inout;
        }
        meshcore_node_exec_cli(&s_node, &s_current_cfg, &s_stats, &env,
                               cmd, cfg_inout, cfg_changed, reply, reply_sz);
        stats_unlock();
    } else {
        snprintf(reply, reply_sz, "Busy.\n");
    }

    if (cli_ctx.ble_name_changed) {
        meshcore_ble_update_name(cli_ctx.new_ble_name);
    }
    if (cli_ctx.ble_pin_changed) {
        meshcore_ble_update_pin(cli_ctx.new_ble_pin);
    }
}

bool lora_widget_ble_connected(void)
{
    return meshcore_ble_is_connected();
}

void lora_widget_prepare_light_sleep(void)
{
    flush_dirty_contacts_if_due(true, 0);
    flush_dirty_msgs_if_due(true, 0);
    bool stop_ble = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        if (s_stats.enabled) {
            stop_ble = true;
            s_ble_paused_for_sleep = true;
        }
        stats_unlock();
    }
    if (stop_ble) {
        meshcore_ble_stop();
    }
}

void lora_widget_resume_ble(void)
{
    lora_config_t cfg_copy;
    uint32_t active_pin = 0;
    bool should_start = false;
    if (stats_lock(pdMS_TO_TICKS(50))) {
        if (s_ble_paused_for_sleep && s_stats.enabled && s_current_cfg.enabled) {
            s_ble_paused_for_sleep = false;
            cfg_copy = s_current_cfg;
            active_pin = (cfg_copy.ble_pin >= 100000 && cfg_copy.ble_pin <= 999999)
                             ? cfg_copy.ble_pin
                             : get_default_ble_pin();
            should_start = true;
        }
        stats_unlock();
    }
    if (should_start) {
        meshcore_ble_init(cfg_copy.node_name, active_pin, on_ble_rx);
        meshcore_ble_set_subscribe_cb(on_ble_subscribed);
        meshcore_ble_set_conn_cb(on_ble_conn_changed);
    }
}

esp_err_t lora_widget_get_channel(uint8_t idx, companion_channel_t *out)
{
    if (!out || idx >= COMPANION_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_companion_loaded();
    if (!companion_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_companion_ctx.channels[idx];
    companion_unlock();
    return ESP_OK;
}

esp_err_t lora_widget_set_channel(uint8_t idx, bool enabled, const char *name, const uint8_t secret[16])
{
    if (idx >= COMPANION_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (idx == 0 && !enabled) {
        return ESP_ERR_INVALID_ARG;
    }
    if (enabled && (!name || !secret)) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_companion_loaded();
    if (!companion_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    companion_channel_t *ch = &s_companion_ctx.channels[idx];
    if (enabled) {
        memset(ch->name, 0, sizeof(ch->name));
        snprintf(ch->name, sizeof(ch->name), "%s", name);
        memcpy(ch->secret, secret, 16);
        ch->valid = true;
    } else {
        memset(ch, 0, sizeof(*ch));
        ch->valid = false;
    }
    if (storage_ready()) {
        save_channels_to_storage(&s_companion_ctx);
    }
    companion_unlock();

    if (idx == 0 && enabled) {
        char b64_psk[32];
        util_base64_encode(secret, 16, b64_psk, sizeof(b64_psk));
        uint8_t secret32[32];
        uint8_t ch_hash = 0;
        bool has_hash = meshcore_channel_key_from_psk(b64_psk, secret32, &ch_hash);
        if (stats_lock(pdMS_TO_TICKS(100))) {
            snprintf(s_current_cfg.channel_name, sizeof(s_current_cfg.channel_name), "%s", name);
            snprintf(s_current_cfg.channel_psk, sizeof(s_current_cfg.channel_psk), "%s", b64_psk);
            snprintf(s_stats.channel_name, sizeof(s_stats.channel_name), "%s", name);
            if (has_hash) {
                s_stats.channel_hash = ch_hash;
            }
            stats_unlock();
        }
    }
    return ESP_OK;
}

size_t lora_widget_get_contact_count(void)
{
    ensure_companion_loaded();
    size_t cnt = 0;
    if (companion_lock(pdMS_TO_TICKS(100))) {
        cnt = s_companion_ctx.contact_count;
        companion_unlock();
    }
    return cnt;
}

esp_err_t lora_widget_get_contact(size_t idx, companion_contact_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_companion_loaded();
    if (!companion_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    if (idx >= s_companion_ctx.contact_count) {
        companion_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_companion_ctx.contacts[idx];
    companion_unlock();
    return ESP_OK;
}

esp_err_t lora_widget_save_contact(const companion_contact_t *contact)
{
    if (!contact) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_companion_loaded();
    if (!companion_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    bool ok = meshcore_companion_add_update_contact(&s_companion_ctx, contact);
    if (ok && storage_ready()) {
        save_contacts_to_storage(&s_companion_ctx);
    }
    companion_unlock();
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t lora_widget_remove_contact(const uint8_t pub_key[32])
{
    if (!pub_key) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_companion_loaded();
    if (!companion_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    bool removed = meshcore_companion_remove_contact(&s_companion_ctx, pub_key);
    if (removed && storage_ready()) {
        save_contacts_to_storage(&s_companion_ctx);
    }
    companion_unlock();
    if (removed) {
        remove_advert_by_pubkey(pub_key);
    }
    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t lora_widget_reset_contact_path(const uint8_t pub_key[32])
{
    if (!pub_key) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_companion_loaded();
    if (!companion_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    companion_contact_t *c = NULL;
    for (size_t i = 0; i < s_companion_ctx.contact_count; i++) {
        if (memcmp(s_companion_ctx.contacts[i].pub_key, pub_key, 32) == 0) {
            c = &s_companion_ctx.contacts[i];
            break;
        }
    }
    if (!c) {
        companion_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    c->out_path_len = -1;
    memset(c->out_path, 0, sizeof(c->out_path));
    c->lastmod = (uint32_t)time(NULL);
    if (storage_ready()) {
        save_contacts_to_storage(&s_companion_ctx);
    }
    companion_unlock();
    return ESP_OK;
}

esp_err_t lora_widget_send_direct_msg(const uint8_t pub_key[32], const char *text, bool force_flood)
{
    return send_direct_msg_internal(pub_key, 0, 0, text, force_flood, NULL);
}

esp_err_t lora_widget_clear_messages(void)
{
    init_sync_primitives();
    ensure_node_msgs_loaded();
    if (!stats_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    memset(s_node.messages, 0, sizeof(s_node.messages));
    s_stats.msg_count = 0;
    s_stats.msg_seq++;
    memset(s_node.pending_acks, 0, sizeof(s_node.pending_acks));
    s_node.pending_ack_next = 0;
    mark_msgs_dirty_locked(0);
    stats_unlock();

    flush_dirty_msgs_if_due(true, 0);
    if (s_rx_notify_cb) {
        s_rx_notify_cb();
    }
    return ESP_OK;
}
