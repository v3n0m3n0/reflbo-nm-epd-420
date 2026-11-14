#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app_internal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lora_widget.h"
#include "nvs.h"

/* The PV forecast's and the house's keys in NVS `secrets` (spec §14.2, M6d): written from the web UI,
 * never returned or logged; a factory reset erases them with the namespace. NVS must be up: a routine
 * deep-sleep wake brings it up through app_net_init() before a sync (gotcha 31). */

static const char *TAG = "app_secrets";

#define NAMESPACE "secrets"
#define KEY_LORA_SEED "lora_seed"
#define KEY_LORA_PIN  "lora_pin"
#define KEY_LORA_V2   "lora_id_v2"

bool app_secret_read(settings_secret_t which, char *out, size_t size, bool *nvs_ready)
{
    out[0] = '\0';
    nvs_handle_t nvs;
    esp_err_t open_err = nvs_open(NAMESPACE, NVS_READONLY, &nvs);
    if (nvs_ready != NULL) {
        *nvs_ready = open_err != ESP_ERR_NVS_NOT_INITIALIZED;
    }
    if (open_err != ESP_OK) {
        return false;
    }
    size_t n = size;
    esp_err_t err = nvs_get_str(nvs, settings_secret_key(which), out, &n);
    nvs_close(nvs);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return out[0] != '\0';
}

bool app_secret_get(settings_secret_t which, char *out, size_t size)
{
    return app_secret_read(which, out, size, NULL);
}

bool app_secret_set(settings_secret_t which)
{
    char value[SETTINGS_SECRET_LEN];
    bool set = app_secret_get(which, value, sizeof(value));
    memset(value, 0, sizeof(value));
    return set;
}

esp_err_t app_secrets_apply(const settings_secrets_t *secrets)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open: %s", esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < SETTINGS_SECRET_COUNT && err == ESP_OK; i++) {
        if (!secrets->given[i]) {
            continue;
        }
        const char *key = settings_secret_key((settings_secret_t)i);
        if (secrets->value[i][0] != '\0') {
            err = nvs_set_str(nvs, key, secrets->value[i]);
        } else {
            err = nvs_erase_key(nvs, key);
            err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err; /* clearing one that isn't there */
        }
        ESP_LOGI(TAG, "%s %s", key, secrets->value[i][0] != '\0' ? "set" : "cleared"); /* never the value */
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write: %s", esp_err_to_name(err));
    } else {
        if (settings_secrets_solcast(secrets)) {
            app_solar_budget_reset(); /* a new key or site: the next step or check asks at once (spec §11.5) */
        }
        if (settings_secrets_solax_dev(secrets)) {
            app_solar_dev_reset(); /* another application, maybe another account: log in and look again (D37) */
        }
    }
    return err;
}

esp_err_t app_lora_sync_psk_secret(const char *psk)
{
    settings_secrets_t s = { 0 };
    s.given[SETTINGS_SECRET_LORA_PSK] = true;
    if (psk != NULL && psk[0] != '\0' && strcmp(psk, LORA_DEFAULT_PUBLIC_PSK) != 0) {
        snprintf(s.value[SETTINGS_SECRET_LORA_PSK], sizeof(s.value[SETTINGS_SECRET_LORA_PSK]), "%s", psk);
    }
    esp_err_t err = app_secrets_apply(&s);
    memset(&s, 0, sizeof(s));
    return err;
}

static uint32_t gen_random_pin(void)
{
    return 100000u + (esp_random() % 900000u);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool has_lora_disk_state(void)
{
    return file_exists("/fs/state/lora_contacts.bin") || file_exists("/fs/state/lora_channels.bin");
}

void app_lora_identity_init(bool has_prior_state)
{
    uint8_t seed[32];
    size_t seed_len = sizeof(seed);
    uint32_t pin = 0;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lora identity open: %s", esp_err_to_name(err));
        lora_widget_set_identity(NULL, 0, true);
        return;
    }

    if (nvs_get_blob(nvs, KEY_LORA_SEED, seed, &seed_len) == ESP_OK && seed_len == sizeof(seed)) {
        if (nvs_get_u32(nvs, KEY_LORA_PIN, &pin) != ESP_OK || pin < 100000u || pin > 999999u) {
            pin = gen_random_pin();
            if (nvs_set_u32(nvs, KEY_LORA_PIN, pin) == ESP_OK) {
                nvs_commit(nvs);
            }
        }
        nvs_close(nvs);
        lora_widget_set_identity(seed, pin, false);
        memset(seed, 0, sizeof(seed));
        return;
    }

    if (has_prior_state || has_lora_disk_state()) {
        nvs_close(nvs);
        ESP_LOGW(TAG, "legacy MAC-derived LoRa identity kept on existing device");
        lora_widget_set_identity(NULL, 0, true);
        return;
    }

    esp_fill_random(seed, sizeof(seed));
    pin = gen_random_pin();
    err = nvs_set_blob(nvs, KEY_LORA_SEED, seed, sizeof(seed));
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, KEY_LORA_PIN, pin);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, KEY_LORA_V2, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        lora_widget_set_identity(seed, pin, false);
        ESP_LOGI(TAG, "generated random NVS LoRa identity");
    } else {
        ESP_LOGE(TAG, "lora identity write: %s", esp_err_to_name(err));
        lora_widget_set_identity(NULL, 0, true);
    }
    memset(seed, 0, sizeof(seed));
}

esp_err_t app_lora_rekey(void)
{
    uint8_t seed[32];
    esp_fill_random(seed, sizeof(seed));
    uint32_t pin = gen_random_pin();

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        memset(seed, 0, sizeof(seed));
        ESP_LOGE(TAG, "rekey open: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(nvs, KEY_LORA_SEED, seed, sizeof(seed));
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, KEY_LORA_PIN, pin);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, KEY_LORA_V2, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        lora_widget_set_identity(seed, pin, false);
        app_lora_apply();
        ESP_LOGI(TAG, "rotated NVS LoRa identity");
    } else {
        ESP_LOGE(TAG, "rekey write: %s", esp_err_to_name(err));
    }
    memset(seed, 0, sizeof(seed));
    return err;
}
