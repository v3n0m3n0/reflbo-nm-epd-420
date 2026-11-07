#include "meshcore_ble.h"
#include <string.h>

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "esp_err.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "meshcore_ble";

/* Nordic UART Service (NUS) UUIDs */
static const ble_uuid128_t s_nus_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_nus_chr_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_nus_chr_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_tx_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type;
static bool     s_connected;
static bool     s_subscribed;
static bool     s_initialized;
static uint16_t s_mtu = 23;
static uint32_t s_ble_pin;
static char     s_node_name[32] = "NM-EPD-420";
static meshcore_ble_rx_cb_t s_rx_cb;

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_advertising(void);

static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len > 0) {
            uint8_t rx_buf[256];
            uint16_t copied = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buf, sizeof(rx_buf), &copied);
            if (rc == 0 && copied > 0 && s_rx_cb) {
                s_rx_cb(rx_buf, copied);
            }
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* RX Characteristic: Write, Write Without Response, Encrypted */
                .uuid = &s_nus_chr_rx_uuid.u,
                .access_cb = nus_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                /* TX Characteristic: Read, Notify, Encrypted */
                .uuid = &s_nus_chr_tx_uuid.u,
                .access_cb = nus_tx_access,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
            },
            { 0 }
        },
    },
    { 0 }
};

static void setup_advertising_data(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_nus_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
    }

    /* Scan response packet with device name */
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    char full_name[64];
    snprintf(full_name, sizeof(full_name), "MeshCore-%s", s_node_name);

    rsp_fields.name = (uint8_t *)full_name;
    rsp_fields.name_len = (uint8_t)strlen(full_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp set fields failed: %d", rc);
    }
}

static void start_advertising(void)
{
    setup_advertising_data();

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started as 'MeshCore-%s'", s_node_name);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "BLE connected, handle=%u", s_conn_handle);
            /* Initiate security pairing */
            ble_gap_security_initiate(s_conn_handle);
        } else {
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        s_connected = false;
        s_subscribed = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        s_subscribed = event->subscribe.cur_notify;
        ESP_LOGI(TAG, "BLE subscribe notify=%d", s_subscribed);
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey;
        memset(&pkey, 0, sizeof(pkey));
        pkey.action = event->passkey.params.action;

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.passkey = s_ble_pin;
            ESP_LOGI(TAG, "BLE passkey display requested: %06lu", (unsigned long)s_ble_pin);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.numcmp_accept = 1;
            ESP_LOGI(TAG, "BLE numcmp accept auto-confirmed");
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else {
            ESP_LOGW(TAG, "BLE unhandled passkey action: %d", event->passkey.params.action);
        }
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption change, status=%d", event->enc_change.status);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "BLE MTU updated to %u", s_mtu);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }
    start_advertising();
}

static void nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t meshcore_ble_init(const char *node_name, uint32_t pin, meshcore_ble_rx_cb_t rx_cb)
{
    if (s_initialized) {
        meshcore_ble_update_pin(pin);
        meshcore_ble_update_name(node_name);
        s_rx_cb = rx_cb;
        return ESP_OK;
    }

    if (node_name && node_name[0] != '\0') {
        strncpy(s_node_name, node_name, sizeof(s_node_name) - 1);
        s_node_name[sizeof(s_node_name) - 1] = '\0';
    }
    s_ble_pin = pin;
    s_rx_cb = rx_cb;

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return ESP_FAIL;
    }

    /* Configure Security Manager (SM) */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    char full_name[64];
    snprintf(full_name, sizeof(full_name), "MeshCore-%s", s_node_name);
    ble_svc_gap_device_name_set(full_name);
    nimble_port_freertos_init(nimble_host_task);

    s_initialized = true;
    ESP_LOGI(TAG, "MeshCore BLE initialized with PIN %06lu", (unsigned long)s_ble_pin);
    return ESP_OK;
}

void meshcore_ble_stop(void)
{
    if (!s_initialized) return;

    ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    s_connected = false;
    s_subscribed = false;

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    }
    s_initialized = false;
    ESP_LOGI(TAG, "MeshCore BLE stopped");
}

bool meshcore_ble_is_connected(void)
{
    return s_connected;
}

bool meshcore_ble_send(const uint8_t *data, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || len == 0 || !data) {
        return false;
    }

    /* Send payload in notification(s) */
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        /* Default ATT MTU header overhead is 3 bytes */
        size_t max_chunk = (s_mtu > 3) ? (s_mtu - 3) : 20;
        if (chunk > max_chunk) chunk = max_chunk;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(&data[offset], chunk);
        if (!om) {
            ESP_LOGE(TAG, "ble_hs_mbuf_from_flat failed");
            return false;
        }

        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gatts_notify_custom failed: %d", rc);
            return false;
        }
        offset += chunk;
    }
    return true;
}

void meshcore_ble_update_pin(uint32_t pin)
{
    s_ble_pin = pin;
}

void meshcore_ble_update_name(const char *node_name)
{
    if (!node_name || node_name[0] == '\0') return;
    strncpy(s_node_name, node_name, sizeof(s_node_name) - 1);
    s_node_name[sizeof(s_node_name) - 1] = '\0';
    char full_name[64];
    snprintf(full_name, sizeof(full_name), "MeshCore-%s", s_node_name);
    ble_svc_gap_device_name_set(full_name);
    if (s_initialized && !s_connected) {
        setup_advertising_data();
    }
}

#else /* !ESP_PLATFORM (Host Stubs) */

esp_err_t meshcore_ble_init(const char *node_name, uint32_t pin, meshcore_ble_rx_cb_t rx_cb)
{
    (void)node_name;
    (void)pin;
    (void)rx_cb;
    return ESP_OK;
}

void meshcore_ble_stop(void)
{
}

bool meshcore_ble_is_connected(void)
{
    return false;
}

bool meshcore_ble_send(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return false;
}

void meshcore_ble_update_pin(uint32_t pin)
{
    (void)pin;
}

void meshcore_ble_update_name(const char *node_name)
{
    (void)node_name;
}

#endif /* ESP_PLATFORM */
