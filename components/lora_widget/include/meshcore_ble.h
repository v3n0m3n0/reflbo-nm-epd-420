#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*meshcore_ble_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*meshcore_ble_subscribe_cb_t)(void);
typedef void (*meshcore_ble_conn_cb_t)(bool connected);

esp_err_t meshcore_ble_init(const char *node_name, uint32_t pin, meshcore_ble_rx_cb_t rx_cb);
void meshcore_ble_stop(void);
bool meshcore_ble_is_connected(void);
bool meshcore_ble_send(const uint8_t *data, size_t len);
void meshcore_ble_update_pin(uint32_t pin);
void meshcore_ble_update_name(const char *node_name);
void meshcore_ble_set_subscribe_cb(meshcore_ble_subscribe_cb_t cb);
void meshcore_ble_set_conn_cb(meshcore_ble_conn_cb_t cb);

#ifdef __cplusplus
}
#endif
