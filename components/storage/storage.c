#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#define BASE_PATH "/fs"
#define PROBE_PATH BASE_PATH "/state/.probe"

static const char *TAG = "storage";
static bool s_ready;
static bool s_corrupt;

static void ensure_dirs(void)
{
    static const char *const k_dirs[] = { BASE_PATH "/cfg", BASE_PATH "/state" };
    for (size_t i = 0; i < sizeof(k_dirs) / sizeof(k_dirs[0]); i++) {
        if (mkdir(k_dirs[i], 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s: errno %d", k_dirs[i], errno);
        }
    }
}

static bool probe_writable(int *errno_out)
{
    if (errno_out) *errno_out = 0;
    FILE *f = fopen(PROBE_PATH, "wb");
    if (f == NULL) {
        if (errno_out) *errno_out = errno;
        return false;
    }
    bool ok = fwrite("1", 1, 1, f) == 1 && fflush(f) == 0;
    int saved_err = errno;
    if (fclose(f) != 0 && ok) {
        ok = false;
        saved_err = errno;
    }
    unlink(PROBE_PATH);
    if (!ok && errno_out) {
        *errno_out = saved_err;
    }
    return ok;
}

esp_err_t storage_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_vfs_littlefs_conf_t conf = {
        .base_path = BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true, /* the first boot finds a blank partition (spec §14.1) */
    };
    ESP_RETURN_ON_ERROR(esp_vfs_littlefs_register(&conf), TAG, "mount");
    ensure_dirs();
    size_t total = 0, used = 0;
    esp_err_t info_err = esp_littlefs_info("storage", &total, &used);
    if (info_err != ESP_OK || (total > 0 && used >= total)) {
        int probe_err = 0;
        if (!probe_writable(&probe_err) || info_err != ESP_OK) {
            ESP_LOGE(TAG, "LittleFS corrupt on mount (info=%s, used=%u/%u KB, errno=%d); formatting",
                     esp_err_to_name(info_err), (unsigned)(used / 1024), (unsigned)(total / 1024), probe_err);
            esp_vfs_littlefs_unregister("storage");
            ESP_RETURN_ON_ERROR(esp_littlefs_format("storage"), TAG, "format");
            ESP_RETURN_ON_ERROR(esp_vfs_littlefs_register(&conf), TAG, "remount");
            ensure_dirs();
            total = 0;
            used = 0;
            esp_littlefs_info("storage", &total, &used);
        }
    }
    ESP_LOGI(TAG, "LittleFS: %u of %u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));
    s_corrupt = false;
    s_ready = true;
    return ESP_OK;
}

bool storage_ready(void)
{
    return s_ready;
}

bool storage_is_healthy(void)
{
    return s_ready && !s_corrupt;
}

bool storage_get_usage(uint32_t *used_kb, uint32_t *total_kb)
{
    if (used_kb) *used_kb = 0;
    if (total_kb) *total_kb = 0;
    if (!s_ready) {
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) != ESP_OK) {
        return false;
    }
    if (used_kb) *used_kb = (uint32_t)(used / 1024U);
    if (total_kb) *total_kb = (uint32_t)(total / 1024U);
    return true;
}

esp_err_t storage_erase(void)
{
    if (s_ready) {
        esp_vfs_littlefs_unregister("storage");
        s_ready = false;
    }
    s_corrupt = false;
    return esp_littlefs_format("storage");
}

esp_err_t storage_load(const char *path, char *buf, size_t size, storage_parse_t parse, void *ctx, bool *from_backup)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not mounted");
    unsigned rejected = 0;
    storage_file_result_t r = storage_file_load(path, buf, size, parse, ctx, from_backup, &rejected);
    if (rejected & STORAGE_REJECTED_MAIN) {
        ESP_LOGW(TAG, "%s rejected", path);
    }
    if (rejected & STORAGE_REJECTED_BACKUP) {
        ESP_LOGW(TAG, "%s.bak rejected", path);
    }
    switch (r) {
    case STORAGE_FILE_OK:
        return ESP_OK;
    case STORAGE_FILE_MISSING:
        return ESP_ERR_NOT_FOUND;
    case STORAGE_FILE_INVALID:
        return ESP_ERR_INVALID_RESPONSE;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t storage_write_atomic(const char *path, const char *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not mounted");
    storage_file_result_t r = storage_file_write_atomic(path, data, len);
    ESP_RETURN_ON_FALSE(r != STORAGE_FILE_BAD_ARG, ESP_ERR_INVALID_ARG, TAG, "path %s", path);
    if (r != STORAGE_FILE_OK && (errno == EBADMSG || errno == EIO)) {
        s_corrupt = true;
    }
    ESP_RETURN_ON_FALSE(r == STORAGE_FILE_OK, ESP_FAIL, TAG, "write %s: errno %d", path, errno);
    return ESP_OK;
}
