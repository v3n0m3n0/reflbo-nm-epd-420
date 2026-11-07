#include "lora_widget.h"
#include "meshcore_proto.h"
#include "meshcore_companion.h"
#include "meshcore_ble.h"
#include "util_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lora_widget";

static spi_device_handle_t s_lora_spi;
static lora_stats_t s_stats;
static meshcore_advert_t s_adverts[MESHCORE_MAX_ADVERTS];
static meshcore_neighbor_t s_neighbors[MESHCORE_MAX_NEIGHBORS];
static meshcore_msg_t s_messages[MESHCORE_MAX_MESSAGES];
static SemaphoreHandle_t s_stats_mutex;
static TaskHandle_t s_task;
static lora_config_t s_current_cfg;

static int32_t s_lat_e6 = 0;
static int32_t s_lon_e6 = 0;
static bool s_has_loc = false;

static uint16_t s_batt_mv = 4190;
static uint8_t s_batt_pct = 99;

#define SEEN_PKT_CACHE_SIZE 64
#define SEEN_PKT_TTL_MS     120000U /* 2 minutes */
typedef struct {
    uint8_t hash[8];
    uint32_t seen_ms;
    bool valid;
} seen_pkt_entry_t;
static seen_pkt_entry_t s_seen_pkts[SEEN_PKT_CACHE_SIZE];
static size_t s_seen_pkt_next = 0;

static void mark_packet_seen(const meshcore_packet_t *pkt, uint32_t now_ms)
{
    if (!pkt) return;
    uint8_t h[8];
    meshcore_packet_hash(pkt, h);
    for (size_t i = 0; i < SEEN_PKT_CACHE_SIZE; i++) {
        if (s_seen_pkts[i].valid && memcmp(s_seen_pkts[i].hash, h, 8) == 0) {
            s_seen_pkts[i].seen_ms = now_ms;
            return;
        }
    }
    s_seen_pkts[s_seen_pkt_next].valid = true;
    s_seen_pkts[s_seen_pkt_next].seen_ms = now_ms;
    memcpy(s_seen_pkts[s_seen_pkt_next].hash, h, 8);
    s_seen_pkt_next = (s_seen_pkt_next + 1) % SEEN_PKT_CACHE_SIZE;
}

static bool is_packet_seen_and_mark(const meshcore_packet_t *pkt, uint32_t now_ms)
{
    if (!pkt) return false;
    uint8_t h[8];
    meshcore_packet_hash(pkt, h);
    for (size_t i = 0; i < SEEN_PKT_CACHE_SIZE; i++) {
        if (s_seen_pkts[i].valid && memcmp(s_seen_pkts[i].hash, h, 8) == 0) {
            if ((now_ms - s_seen_pkts[i].seen_ms) < SEEN_PKT_TTL_MS) {
                return true;
            }
            s_seen_pkts[i].seen_ms = now_ms;
            return false;
        }
    }
    s_seen_pkts[s_seen_pkt_next].valid = true;
    s_seen_pkts[s_seen_pkt_next].seen_ms = now_ms;
    memcpy(s_seen_pkts[s_seen_pkt_next].hash, h, 8);
    s_seen_pkt_next = (s_seen_pkt_next + 1) % SEEN_PKT_CACHE_SIZE;
    return false;
}

typedef struct {
    bool pending_channel_txt;
    uint32_t msg_ts;
    char text[128];
    bool pending_advert;
    bool advert_flood;
    bool pending_ack_confirm;
    uint8_t ack_tag[4];
} pending_ble_tx_t;
static pending_ble_tx_t s_pending_ble_tx;

static uint8_t s_pub_key[32];
static uint8_t s_prv_key[64];
static bool s_keys_initialized = false;

#define COMPANION_PUSH_QUEUE_SIZE 8
typedef struct {
    uint8_t len;
    uint8_t data[COMPANION_MAX_FRAME_SIZE];
} companion_frame_t;

static companion_context_t s_companion_ctx;
static bool s_companion_initialized = false;
static companion_frame_t s_push_queue[COMPANION_PUSH_QUEUE_SIZE];
static size_t s_push_head = 0;
static size_t s_push_tail = 0;
static size_t s_push_count = 0;
static SemaphoreHandle_t s_push_mutex = NULL;

#define COMPANION_MSG_QUEUE_SIZE 8
static companion_frame_t s_msg_queue[COMPANION_MSG_QUEUE_SIZE];
static size_t s_msg_head = 0;
static size_t s_msg_tail = 0;
static size_t s_msg_count = 0;
static SemaphoreHandle_t s_msg_mutex = NULL;

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

    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_efuse_mac_get_default failed (%s), using random seed", esp_err_to_name(err));
        esp_fill_random(mac, sizeof(mac));
    }

    /* Seed = SHA256("meshcore-ed25519-v1" || mac[6]) */
    uint8_t seed_input[19 + 6];
    memcpy(seed_input, "meshcore-ed25519-v1", 19);
    memcpy(seed_input + 19, mac, 6);

    uint8_t seed[32];
    util_sha256(seed_input, sizeof(seed_input), seed);

    meshcore_ed25519_create_keypair(s_pub_key, s_prv_key, seed);

    for (int i = 0; i < 32; i++) {
        snprintf(s_stats.pub_key_hex + (i * 2), 3, "%02x", s_pub_key[i]);
    }
    s_stats.pub_key_hex[64] = '\0';
    s_keys_initialized = true;

    ESP_LOGI(TAG, "MeshCore Ed25519 Node Key derived. PubKey prefix: %.16s...", s_stats.pub_key_hex);
}

static uint32_t get_default_ble_pin(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    return 100000u + ((((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5]) % 900000u);
}

static bool lora_wait_busy(uint32_t timeout_ms)
{
    uint32_t waited_us = 0;
    uint32_t max_us = timeout_ms * 1000;
    while (gpio_get_level(BOARD_PIN_LORA_BUSY) == 1) {
        esp_rom_delay_us(50);
        waited_us += 50;
        if (waited_us >= max_us) {
            return false;
        }
    }
    return true;
}

static esp_err_t lora_write_cmd(uint8_t cmd, const uint8_t *params, size_t len)
{
    if (!lora_wait_busy(50)) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t);
    if (err == ESP_OK && len > 0 && params != NULL) {
        spi_transaction_t t2 = {
            .length = 8 * len,
            .tx_buffer = params,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t2);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    return err;
}

static esp_err_t lora_read_cmd(uint8_t cmd, uint8_t *out, size_t len)
{
    if (!lora_wait_busy(50)) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t);

    /* Read status byte */
    uint8_t dummy = 0;
    spi_transaction_t td = {
        .length = 8,
        .tx_buffer = &dummy,
    };
    err = spi_device_polling_transmit(s_lora_spi, &td);

    if (err == ESP_OK && len > 0 && out != NULL) {
        uint8_t zeros[8] = {0};
        spi_transaction_t t2 = {
            .length = 8 * len,
            .tx_buffer = (len <= sizeof(zeros)) ? zeros : NULL,
            .rx_buffer = out,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t2);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    return err;
}

static esp_err_t lora_write_reg(uint16_t reg, uint8_t val)
{
    uint8_t data[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return lora_write_cmd(0x0D, data, 3);
}

static esp_err_t lora_write_buffer(uint8_t offset, const uint8_t *data, size_t len)
{
    if (!lora_wait_busy(50)) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    uint8_t cmd_hdr[2] = { 0x0E, offset };
    spi_transaction_t t_hdr = {
        .length = 16,
        .tx_buffer = cmd_hdr,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t_hdr);
    if (err == ESP_OK && len > 0 && data != NULL) {
        spi_transaction_t t_data = {
            .length = 8 * len,
            .tx_buffer = data,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t_data);
    }

    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    return err;
}

static esp_err_t lora_read_buffer(uint8_t offset, uint8_t *out, size_t len)
{
    if (!lora_wait_busy(50)) {
        return ESP_ERR_TIMEOUT;
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);

    uint8_t cmd_hdr[3] = { 0x1E, offset, 0x00 /* dummy */ };
    spi_transaction_t t_hdr = {
        .length = 24,
        .tx_buffer = cmd_hdr,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_spi, &t_hdr);
    if (err == ESP_OK && len > 0 && out != NULL) {
        spi_transaction_t t_data = {
            .length = 8 * len,
            .tx_buffer = NULL,
            .rx_buffer = out,
        };
        err = spi_device_polling_transmit(s_lora_spi, &t_data);
    }

    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    esp_rom_delay_us(10);
    lora_wait_busy(50);
    return err;
}

static uint8_t sx1262_bw_register(uint32_t bw_hz)
{
    if (bw_hz <= 7800)   return 0x00; /* 7.81 kHz */
    if (bw_hz <= 10400)  return 0x08; /* 10.42 kHz */
    if (bw_hz <= 15600)  return 0x01; /* 15.63 kHz */
    if (bw_hz <= 20800)  return 0x09; /* 20.83 kHz */
    if (bw_hz <= 31250)  return 0x02; /* 31.25 kHz */
    if (bw_hz <= 41700)  return 0x0A; /* 41.67 kHz */
    if (bw_hz <= 62500)  return 0x03; /* 62.50 kHz */
    if (bw_hz <= 125000) return 0x04; /* 125.00 kHz */
    if (bw_hz <= 250000) return 0x05; /* 250.00 kHz */
    return 0x06;                       /* 500.00 kHz */
}

static void IRAM_ATTR lora_dio1_isr_handler(void *arg)
{
    TaskHandle_t task = (TaskHandle_t)arg;
    if (task != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static esp_err_t sx1262_transmit(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_stats.initialized || !s_stats.enabled || s_lora_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Temporarily mask DIO1 interrupt so background RX task does not clear TxDone */
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);

    /* Standby */
    uint8_t standby = 0x00;
    lora_write_cmd(0x80, &standby, 1);

    /* Set buffer base address: TX at 0 */
    uint8_t base_addr[2] = { 0x00, 0x00 };
    lora_write_cmd(0x8F, base_addr, 2);

    /* Write data into FIFO */
    esp_err_t err = lora_write_buffer(0x00, data, len);
    if (err != ESP_OK) {
        gpio_intr_enable(BOARD_PIN_LORA_DIO1);
        xSemaphoreGive(s_stats_mutex);
        return err;
    }

    /* Configure PacketParams for TX */
    uint16_t preamble = (s_current_cfg.sf <= 8) ? 32 : 16;
    uint8_t pkt_params[6] = {
        (uint8_t)(preamble >> 8),
        (uint8_t)(preamble & 0xFF),
        0x00, /* Explicit header */
        (uint8_t)len,
        0x01, /* CRC on */
        0x00  /* Standard IQ */
    };
    lora_write_cmd(0x8C, pkt_params, 6);

    /* Set DIO IRQ: TxDone(0x0001) | Timeout(0x0200) in IrqMask; DIO1 mask 0x0000 */
    uint8_t dio_irq[8] = { 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    lora_write_cmd(0x08, dio_irq, 8);

    /* Clear IRQ */
    uint8_t clr[2] = { 0x03, 0xFF };
    lora_write_cmd(0x02, clr, 2);

    /* Calculate airtime */
    uint32_t bw_hz = s_current_cfg.bw_hz ? s_current_cfg.bw_hz : (uint32_t)s_current_cfg.bw_khz * 1000;
    uint32_t air_ms = meshcore_calc_airtime_ms(bw_hz, s_current_cfg.sf, s_current_cfg.cr, preamble, len);

    /* Set TX: timeout 0 (disabled in hardware; software polling handles timeout) */
    uint8_t tx_to[3] = { 0x00, 0x00, 0x00 };
    lora_write_cmd(0x83, tx_to, 3);

    /* Poll for TxDone */
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t max_wait_ms = air_ms * 2 + 1000;
    bool tx_ok = false;

    while (((uint32_t)(esp_timer_get_time() / 1000) - start_ms) < max_wait_ms) {
        uint8_t irq_stat[2] = {0};
        if (lora_read_cmd(0x12, irq_stat, 2) == ESP_OK) {
            uint16_t irq = ((uint16_t)irq_stat[0] << 8) | irq_stat[1];
            if (irq & 0x0001) { /* TxDone */
                tx_ok = true;
                break;
            }
            if (irq & 0x0200) { /* Timeout */
                ESP_LOGW(TAG, "TX timeout IRQ triggered");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (tx_ok) {
        s_stats.packets_sent++;
        s_stats.tx_air_ms += air_ms;
        ESP_LOGI(TAG, "TX done: %u bytes, airtime: %lu ms (total TX air: %lu ms)",
                 (unsigned)len, (unsigned long)air_ms, (unsigned long)s_stats.tx_air_ms);
    } else {
        ESP_LOGE(TAG, "TX failed or timed out");
    }

    /* Restore continuous RX */
    uint8_t rx_pkt_params[6] = {
        (uint8_t)(preamble >> 8),
        (uint8_t)(preamble & 0xFF),
        0x00, /* Explicit header */
        0xFF, /* Max RX payload */
        0x01, /* CRC on */
        0x00  /* Standard IQ */
    };
    lora_write_cmd(0x8C, rx_pkt_params, 6);

    uint8_t rx_dio_irq[8] = { 0x02, 0x42, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
    lora_write_cmd(0x08, rx_dio_irq, 8);
    lora_write_cmd(0x02, clr, 2);

    uint8_t rx_timeout[3] = { 0xFF, 0xFF, 0xFF };
    lora_write_cmd(0x82, rx_timeout, 3);

    gpio_intr_enable(BOARD_PIN_LORA_DIO1);
    xSemaphoreGive(s_stats_mutex);
    return tx_ok ? ESP_OK : ESP_FAIL;
}

static void update_advert_table(const meshcore_advert_t *adv)
{
    /* Find existing advert by pub_key */
    for (int i = 0; i < s_stats.advert_count; i++) {
        if (memcmp(s_adverts[i].pub_key, adv->pub_key, 32) == 0) {
            s_adverts[i] = *adv;
            return;
        }
    }

    /* Append if room */
    if (s_stats.advert_count < MESHCORE_MAX_ADVERTS) {
        s_adverts[s_stats.advert_count++] = *adv;
        return;
    }

    /* Evict oldest */
    int oldest_idx = 0;
    uint32_t oldest_time = s_adverts[0].rx_time_s;
    for (int i = 1; i < MESHCORE_MAX_ADVERTS; i++) {
        if (s_adverts[i].rx_time_s < oldest_time) {
            oldest_time = s_adverts[i].rx_time_s;
            oldest_idx = i;
        }
    }
    s_adverts[oldest_idx] = *adv;
}

static void update_neighbor_table(const meshcore_neighbor_t *nb)
{
    for (int i = 0; i < s_stats.neighbor_count; i++) {
        if (memcmp(s_neighbors[i].pub_key_prefix, nb->pub_key_prefix, 8) == 0) {
            s_neighbors[i] = *nb;
            return;
        }
    }

    if (s_stats.neighbor_count < MESHCORE_MAX_NEIGHBORS) {
        s_neighbors[s_stats.neighbor_count++] = *nb;
        return;
    }

    int oldest_idx = 0;
    uint32_t oldest_time = s_neighbors[0].rx_time_s;
    for (int i = 1; i < MESHCORE_MAX_NEIGHBORS; i++) {
        if (s_neighbors[i].rx_time_s < oldest_time) {
            oldest_time = s_neighbors[i].rx_time_s;
            oldest_idx = i;
        }
    }
    s_neighbors[oldest_idx] = *nb;
}

static void append_message_history(const meshcore_msg_t *msg)
{
    if (s_stats.msg_count < MESHCORE_MAX_MESSAGES) {
        s_messages[s_stats.msg_count++] = *msg;
        return;
    }

    /* Shift ring buffer left and insert at end */
    for (int i = 0; i < MESHCORE_MAX_MESSAGES - 1; i++) {
        s_messages[i] = s_messages[i + 1];
    }
    s_messages[MESHCORE_MAX_MESSAGES - 1] = *msg;
}

bool lora_widget_get_advert(size_t index, meshcore_advert_t *out)
{
    if (out == NULL) return false;
    bool ok = false;
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (index < s_stats.advert_count && index < MESHCORE_MAX_ADVERTS) {
            *out = s_adverts[index];
            ok = true;
        }
        xSemaphoreGive(s_stats_mutex);
    }
    return ok;
}

bool lora_widget_get_neighbor(size_t index, meshcore_neighbor_t *out)
{
    if (out == NULL) return false;
    bool ok = false;
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (index < s_stats.neighbor_count && index < MESHCORE_MAX_NEIGHBORS) {
            *out = s_neighbors[index];
            ok = true;
        }
        xSemaphoreGive(s_stats_mutex);
    }
    return ok;
}

bool lora_widget_get_message(size_t index, meshcore_msg_t *out)
{
    if (out == NULL) return false;
    bool ok = false;
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (index < s_stats.msg_count && index < MESHCORE_MAX_MESSAGES) {
            *out = s_messages[index];
            ok = true;
        }
        xSemaphoreGive(s_stats_mutex);
    }
    return ok;
}

static void lora_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LoRa SX1262 MeshCore monitor task started (interrupt-driven)");

    for (;;) {
        /* Wait up to 200 ms for packet interrupt or BLE companion command */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));

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
            uint8_t resp[COMPANION_MAX_FRAME_SIZE];
            size_t resp_len = lora_widget_handle_companion_cmd(frame.data, frame.len, &s_current_cfg, &cfg_changed, resp, sizeof(resp));
            if (resp_len > 0) {
                meshcore_ble_send(resp, resp_len);
            }
            if (s_pending_ble_tx.pending_ack_confirm) {
                uint8_t ack_push[9];
                ack_push[0] = PUSH_CODE_SEND_CONFIRMED;
                memcpy(&ack_push[1], s_pending_ble_tx.ack_tag, 4);
                uint32_t rtt_ms = 250;
                memcpy(&ack_push[5], &rtt_ms, 4);
                push_companion_event(ack_push, sizeof(ack_push));
                s_pending_ble_tx.pending_ack_confirm = false;
            }
            if (s_pending_ble_tx.pending_channel_txt) {
                s_pending_ble_tx.pending_channel_txt = false;
                lora_widget_send_channel_msg_ts(s_pending_ble_tx.msg_ts, s_pending_ble_tx.text);
            }
            if (s_pending_ble_tx.pending_advert) {
                s_pending_ble_tx.pending_advert = false;
                lora_widget_send_advert(s_pending_ble_tx.advert_flood);
            }
            if (cfg_changed) {
                lora_widget_apply(&s_current_cfg);
            }
        }

        if (!s_stats.enabled || !s_stats.initialized) {
            continue;
        }

        if (notified == 0) {
            static uint8_t s_nf_tick = 0;
            if (++s_nf_tick >= 5) {
                s_nf_tick = 0;
                /* Background timeout: sample instantaneous noise floor */
                uint8_t rssi_raw = 0;
                if (lora_read_cmd(0x15, &rssi_raw, 1) == ESP_OK) {
                    int16_t inst = -(int16_t)(rssi_raw / 2);
                    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        if (s_stats.noise_floor_dbm == 0) {
                            s_stats.noise_floor_dbm = inst;
                        } else {
                            s_stats.noise_floor_dbm = (int16_t)((s_stats.noise_floor_dbm * 7 + inst) / 8);
                        }
                        xSemaphoreGive(s_stats_mutex);
                    }
                }
            }
            continue;
        }

        /* Read IRQ status (cmd 0x12) */
        uint8_t irq_stat[2] = {0};
        if (lora_read_cmd(0x12, irq_stat, 2) != ESP_OK) {
            continue;
        }

        uint16_t irq = ((uint16_t)irq_stat[0] << 8) | irq_stat[1];

        /* Check CRC Error */
        if (irq & 0x0040) {
            if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_stats.recv_errors++;
                xSemaphoreGive(s_stats_mutex);
            }
        }

        /* Bit 1 is RxDone (0x0002) */
        if (irq & 0x0002) {
            /* GetPacketStatus (cmd 0x14) */
            uint8_t pkt_stat[3] = {0};
            int16_t rssi = -120;
            int8_t snr_q4 = 0;
            if (lora_read_cmd(0x14, pkt_stat, 3) == ESP_OK) {
                rssi = -(int16_t)(pkt_stat[0] / 2);
                snr_q4 = (int8_t)pkt_stat[1];
            }

            /* Read RxBufferStatus (cmd 0x13): payload_len, start_offset */
            uint8_t rx_buf_stat[2] = {0};
            uint8_t payload_len = 0;
            uint8_t start_offset = 0;
            if (lora_read_cmd(0x13, rx_buf_stat, 2) == ESP_OK) {
                payload_len = rx_buf_stat[0];
                start_offset = rx_buf_stat[1];
            }

            /* Read packet buffer */
            static uint8_t s_rx_bytes[256];
            bool read_ok = false;
            if (payload_len > 0 && lora_read_buffer(start_offset, s_rx_bytes, payload_len) == ESP_OK) {
                read_ok = true;
            }

            /* Clear IRQ (cmd 0x02, 0x03FF to clear all pending flags) */
            uint8_t clr[2] = { 0x03, 0xFF };
            lora_write_cmd(0x02, clr, 2);

            if (!read_ok) {
                continue;
            }

            uint32_t bw_hz = s_current_cfg.bw_hz ? s_current_cfg.bw_hz : (uint32_t)s_current_cfg.bw_khz * 1000;
            uint16_t preamble = (s_current_cfg.sf <= 8) ? 32 : 16;
            uint32_t air_ms = meshcore_calc_airtime_ms(bw_hz, s_current_cfg.sf, s_current_cfg.cr, preamble, payload_len);

            /* Update stats */
            if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_stats.packet_count++;
                s_stats.last_rssi_dbm = rssi;
                s_stats.last_snr_q4 = snr_q4;
                s_stats.last_snr_db = snr_q4 / 4;
                s_stats.last_packet_time_s = (uint32_t)time(NULL);
                s_stats.rx_air_ms += air_ms;
                xSemaphoreGive(s_stats_mutex);
            }

            /* Parse MeshCore packet */
            static meshcore_packet_t s_pkt;
            if (!meshcore_packet_decode(&s_pkt, s_rx_bytes, payload_len)) {
                ESP_LOGD(TAG, "Non-MeshCore packet received (%u bytes)", (unsigned)payload_len);
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
            if (is_packet_seen_and_mark(&s_pkt, now_ms)) {
                ESP_LOGD(TAG, "Dropping duplicate flood packet (hops=%u)", (unsigned)hops);
                continue;
            }

            if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (route == MESHCORE_ROUTE_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_FLOOD) {
                    s_stats.recv_flood++;
                } else {
                    s_stats.recv_direct++;
                }
                xSemaphoreGive(s_stats_mutex);
            }

            /* Handle payload types */
            if (ptype == MESHCORE_PAYLOAD_ADVERT) {
                static meshcore_advert_t s_adv;
                if (meshcore_parse_advert_payload(s_pkt.payload, s_pkt.payload_len, &s_adv)) {
                    s_adv.hops = hops;
                    s_adv.rssi_dbm = rssi;
                    s_adv.snr_q4 = snr_q4;
                    s_adv.rx_time_s = (uint32_t)time(NULL);

                    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        update_advert_table(&s_adv);
                        strncpy(s_stats.last_sender, s_adv.name, sizeof(s_stats.last_sender) - 1);
                        s_stats.last_sender[sizeof(s_stats.last_sender) - 1] = '\0';
                        xSemaphoreGive(s_stats_mutex);
                    }

                    /* Add/update contact in companion table */
                    if (!s_companion_initialized) {
                        meshcore_companion_init(&s_companion_ctx);
                        s_companion_initialized = true;
                    }
                    companion_contact_t c;
                    memset(&c, 0, sizeof(c));
                    memcpy(c.pub_key, s_adv.pub_key, COMPANION_PUB_KEY_SIZE);
                    c.type = s_adv.node_type;
                    c.flags = 0;
                    c.out_path_len = (int8_t)hops;
                    if (hops > 0 && s_pkt.path_len > 0) {
                        size_t c_path_len = s_pkt.path_len;
                        if (c_path_len > COMPANION_MAX_PATH_SIZE) c_path_len = COMPANION_MAX_PATH_SIZE;
                        memcpy(c.out_path, s_pkt.path, c_path_len);
                    }
                    strncpy(c.name, s_adv.name, sizeof(c.name) - 1);
                    c.last_advert_ts = s_adv.timestamp;
                    c.lat_e6 = s_adv.has_loc ? s_adv.lat_e6 : 0;
                    c.lon_e6 = s_adv.has_loc ? s_adv.lon_e6 : 0;
                    c.lastmod = (uint32_t)time(NULL);
                    meshcore_companion_add_update_contact(&s_companion_ctx, &c);

                    /* Push advert notification to BLE Companion */
                    uint8_t adv_push[1 + COMPANION_PUB_KEY_SIZE];
                    adv_push[0] = PUSH_CODE_ADVERT;
                    memcpy(&adv_push[1], s_adv.pub_key, COMPANION_PUB_KEY_SIZE);
                    push_companion_event(adv_push, sizeof(adv_push));

                    ESP_LOGI(TAG, "Discovered Node Advert: '%s' (type %u, hops %u, RSSI %d dBm)",
                             s_adv.name, s_adv.node_type, s_adv.hops, rssi);
                }
            } else if (ptype == MESHCORE_PAYLOAD_CONTROL) {
                if (s_pkt.payload_len >= 6 && (s_pkt.payload[0] & 0xF0) == 0x80) {
                    /* Discover Request received: reply with Discover Response */
                    uint8_t resp_payload[14];
                    resp_payload[0] = 0x90 | MESHCORE_ADV_CHAT;
                    resp_payload[1] = (uint8_t)snr_q4;
                    memcpy(&resp_payload[2], &s_pkt.payload[2], 4); /* Match request tag */
                    memcpy(&resp_payload[6], s_pub_key, 8);        /* Our pubkey prefix */

                    uint8_t resp_raw[64];
                    meshcore_packet_t resp_pkt;
                    memset(&resp_pkt, 0, sizeof(resp_pkt));
                    resp_pkt.header = (uint8_t)((MESHCORE_ROUTE_DIRECT & 0x03) | ((MESHCORE_PAYLOAD_CONTROL & 0x0F) << 2));
                    resp_pkt.path_len = 0;
                    resp_pkt.payload_len = sizeof(resp_payload);
                    memcpy(resp_pkt.payload, resp_payload, sizeof(resp_payload));
                    size_t raw_sz = meshcore_packet_encode(&resp_pkt, resp_raw, sizeof(resp_raw));
                    if (raw_sz > 0) {
                        /* Short randomized delay before response to prevent collisions */
                        vTaskDelay(pdMS_TO_TICKS(10 + (esp_random() % 30)));
                        sx1262_transmit(resp_raw, raw_sz);
                        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            s_stats.sent_direct++;
                            xSemaphoreGive(s_stats_mutex);
                        }
                    }
                } else if (s_pkt.payload_len >= 14 && (s_pkt.payload[0] & 0xF0) == 0x90) {
                    /* Discover Response received */
                    meshcore_neighbor_t nb;
                    uint32_t tag = 0;
                    if (meshcore_parse_discover_resp(s_pkt.payload, s_pkt.payload_len, &nb, &tag)) {
                        nb.rssi_dbm = rssi;
                        nb.rx_time_s = (uint32_t)time(NULL);
                        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            update_neighbor_table(&nb);
                            xSemaphoreGive(s_stats_mutex);
                        }
                        ESP_LOGI(TAG, "Discover Response from neighbor: prefix %02x%02x%02x%02x (RSSI %d dBm)",
                                 nb.pub_key_prefix[0], nb.pub_key_prefix[1], nb.pub_key_prefix[2], nb.pub_key_prefix[3], rssi);
                    }
                }
            } else if (ptype == MESHCORE_PAYLOAD_GRP_TXT) {
                uint32_t ts = 0;
                char text[160] = {0};
                if (meshcore_parse_grp_txt_payload(s_pkt.payload, s_pkt.payload_len,
                                                   s_current_cfg.channel_psk,
                                                   &ts, text, sizeof(text))) {
                    meshcore_msg_t msg = {
                        .timestamp = ts,
                        .rx_time_s = (uint32_t)time(NULL),
                        .rssi_dbm = rssi,
                        .snr_q4 = snr_q4,
                        .hops = hops,
                        .outgoing = false,
                    };
                    strncpy(msg.text, text, sizeof(msg.text) - 1);

                    bool is_dup_msg = false;
                    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        for (int i = 0; i < s_stats.msg_count && i < MESHCORE_MAX_MESSAGES; i++) {
                            if ((s_messages[i].timestamp == ts || s_messages[i].timestamp == 0 || ts == 0) &&
                                strcmp(s_messages[i].text, text) == 0) {
                                is_dup_msg = true;
                                break;
                            }
                        }
                        if (!is_dup_msg) {
                            append_message_history(&msg);
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
                        xSemaphoreGive(s_stats_mutex);
                    }

                    if (is_dup_msg) {
                        ESP_LOGD(TAG, "Dropping duplicate message payload: '%s'", text);
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
                            mf->data[4] = 0; /* Public channel 0 */
                            mf->data[5] = hops;
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

                    ESP_LOGI(TAG, "Channel Message: '%s'", text);
                }
            }

            /* Client repeat forwarding */
            if (s_current_cfg.client_repeat &&
                (route == MESHCORE_ROUTE_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_FLOOD)) {
                if (hops < 7) {
                    /* Increment hop count (bits 0..5 in path_len_byte) */
                    size_t path_byte_offset = (route == MESHCORE_ROUTE_TRANSPORT_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_DIRECT) ? 5 : 1;
                    if (path_byte_offset < payload_len) {
                        s_rx_bytes[path_byte_offset]++;
                        /* Forward with small randomized delay */
                        vTaskDelay(pdMS_TO_TICKS(20 + (esp_random() % 50)));
                        sx1262_transmit(s_rx_bytes, payload_len);
                        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            s_stats.sent_flood++;
                            xSemaphoreGive(s_stats_mutex);
                        }
                    }
                }
            }
        } else if (irq != 0) {
            /* Clear non-RxDone IRQ flags */
            uint8_t clr[2] = { irq_stat[0], irq_stat[1] };
            lora_write_cmd(0x02, clr, 2);
        }

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
}

static bool companion_send_advert(bool flood)
{
    s_pending_ble_tx.pending_advert = true;
    s_pending_ble_tx.advert_flood = flood;
    return true;
}

static bool companion_send_channel_txt(uint8_t channel_idx, const char *text)
{
    (void)channel_idx;
    s_pending_ble_tx.pending_channel_txt = true;
    s_pending_ble_tx.msg_ts = s_companion_ctx.last_tx_msg_ts;
    if (text) {
        strncpy(s_pending_ble_tx.text, text, sizeof(s_pending_ble_tx.text) - 1);
        s_pending_ble_tx.text[sizeof(s_pending_ble_tx.text) - 1] = '\0';
    } else {
        s_pending_ble_tx.text[0] = '\0';
    }
    return true;
}

static void companion_exec_cli(const char *cmd, char *reply, size_t reply_sz)
{
    lora_widget_exec_cli(cmd, &s_current_cfg, NULL, reply, reply_sz);
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
    if (!s_companion_initialized) {
        meshcore_companion_init(&s_companion_ctx);
        s_companion_initialized = true;
    }

    companion_env_t env = {
        .lat_e6 = s_lat_e6,
        .lon_e6 = s_lon_e6,
        .batt_mv = s_batt_mv,
        .storage_used_kb = 128,
        .storage_total_kb = 4096,
        .get_time_s = companion_get_time,
        .set_time_s = companion_set_time,
        .stats = &s_stats,
        .send_advert = companion_send_advert,
        .send_channel_txt = companion_send_channel_txt,
        .exec_cli = companion_exec_cli,
        .pop_msg = companion_pop_msg,
        .send_frame_cb = companion_send_frame_cb,
    };
    memcpy(env.pub_key, s_pub_key, 32);

    if (cmd[0] == CMD_SEND_TXT_MSG && cmd_len >= 7) {
        s_pending_ble_tx.pending_ack_confirm = true;
        memcpy(s_pending_ble_tx.ack_tag, &cmd[3], 4);
    }

    return meshcore_companion_handle_cmd(&s_companion_ctx, &env,
                                         cfg_inout ? cfg_inout : &s_current_cfg,
                                         cfg_changed,
                                         cmd, cmd_len, resp_out, resp_max);
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

esp_err_t lora_widget_apply(const lora_config_t *cfg)
{
    if (s_stats_mutex == NULL) {
        s_stats_mutex = xSemaphoreCreateMutex();
    }

    init_node_keys();

    uint32_t active_pin = (cfg && cfg->ble_pin >= 100000 && cfg->ble_pin <= 999999) ? cfg->ble_pin : get_default_ble_pin();
    s_stats.ble_pin = active_pin;

    if (cfg == NULL || !cfg->enabled) {
        meshcore_ble_stop();
        gpio_intr_disable(BOARD_PIN_LORA_DIO1);
        if (s_task != NULL) {
            ulTaskNotifyTake(pdTRUE, 0); /* drain stale notifications */
        }
        if (s_stats.initialized && s_lora_spi != NULL) {
            uint8_t sleep_cfg = 0x00; /* cold start (0x84) */
            lora_write_cmd(0x84, &sleep_cfg, 1);
        }
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);

        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.enabled = false;
            s_stats.initialized = false;
            s_stats.ble_pin = active_pin;
            xSemaphoreGive(s_stats_mutex);
        }
        ESP_LOGI(TAG, "LoRa radio disabled");
        return ESP_OK;
    }

    /* Fast path: if radio is already initialized and RF parameters didn't change, skip SX1262 HW reset */
    if (s_stats.initialized && !is_rf_config_changed(&s_current_cfg, cfg)) {
        s_current_cfg = *cfg;
        if (s_current_cfg.node_name[0] == '\0') {
            strncpy(s_current_cfg.node_name, "NM-EPD-420", sizeof(s_current_cfg.node_name) - 1);
        }
        if (s_current_cfg.channel_psk[0] == '\0') {
            strncpy(s_current_cfg.channel_psk, LORA_DEFAULT_PUBLIC_PSK, sizeof(s_current_cfg.channel_psk) - 1);
        }
        uint8_t secret32[32];
        uint8_t ch_hash = 0;
        if (meshcore_channel_key_from_psk(s_current_cfg.channel_psk, secret32, &ch_hash)) {
            s_stats.channel_hash = ch_hash;
        }
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.cad_enabled = cfg->cad_enabled;
            s_stats.client_repeat = cfg->client_repeat;
            s_stats.path_hash_mode = cfg->path_hash_mode;
            s_stats.airtime_factor_x10 = cfg->airtime_factor_x10;
            s_stats.wake_on_rx = cfg->wake_on_rx;
            s_stats.share_loc = cfg->share_loc;
            s_stats.ble_pin = active_pin;
            strncpy(s_stats.node_name, s_current_cfg.node_name, sizeof(s_stats.node_name) - 1);
            strncpy(s_stats.channel_name, s_current_cfg.channel_name, sizeof(s_stats.channel_name) - 1);
            xSemaphoreGive(s_stats_mutex);
        }
        meshcore_ble_init(s_current_cfg.node_name, s_stats.ble_pin, on_ble_rx);
        return ESP_OK;
    }

    s_current_cfg = *cfg;
    if (s_current_cfg.node_name[0] == '\0') {
        strncpy(s_current_cfg.node_name, "NM-EPD-420", sizeof(s_current_cfg.node_name) - 1);
    }
    if (s_current_cfg.channel_psk[0] == '\0') {
        strncpy(s_current_cfg.channel_psk, LORA_DEFAULT_PUBLIC_PSK, sizeof(s_current_cfg.channel_psk) - 1);
    }
    uint8_t secret32[32];
    uint8_t ch_hash = 0;
    if (meshcore_channel_key_from_psk(s_current_cfg.channel_psk, secret32, &ch_hash)) {
        s_stats.channel_hash = ch_hash;
    }

    /* Pause RX and disable interrupts during reconfiguration */
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);
    if (s_task != NULL) {
        ulTaskNotifyTake(pdTRUE, 0); /* drain stale notifications */
    }

    /* Mark uninitialized while reconfiguring */
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.initialized = false;
        xSemaphoreGive(s_stats_mutex);
    }

    /* Configure BOARD_PIN_TF_CS (GPIO7) output HIGH to isolate MicroSD card on shared SPI3 */
    gpio_config_t tf_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_TF_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&tf_conf), TAG, "tf_cs gpio");
    gpio_set_level(BOARD_PIN_TF_CS, 1);

    /* Output pins for SX1262 (NSS, RST, EN) */
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_NSS) | (1ULL << BOARD_PIN_LORA_RST) | (1ULL << BOARD_PIN_LORA_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "output gpio");
    gpio_set_level(BOARD_PIN_LORA_EN, 1);
    gpio_set_level(BOARD_PIN_LORA_NSS, 1);
    gpio_set_level(BOARD_PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Input pins: BUSY (polling only) */
    gpio_config_t busy_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_conf), TAG, "busy gpio");

    /* Input pins: DIO1 interrupt */
    gpio_config_t dio1_conf = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LORA_DIO1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dio1_conf), TAG, "dio1 gpio");

    /* Hardware reset */
    gpio_set_level(BOARD_PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!lora_wait_busy(50)) {
        ESP_LOGW(TAG, "SX1262 BUSY timeout after reset - radio disabled");
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.enabled = true;
            s_stats.initialized = false;
            xSemaphoreGive(s_stats_mutex);
        }
        return ESP_OK;
    }

    /* SPI bus initialization on SPI3 */
    if (s_lora_spi == NULL) {
        spi_bus_config_t buscfg = {
            .miso_io_num = BOARD_PIN_LORA_MISO,
            .mosi_io_num = BOARD_PIN_LORA_MOSI,
            .sclk_io_num = BOARD_PIN_LORA_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 256,
        };
        esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
            return ret;
        }

        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = 4 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = -1,
            .queue_size = 1,
        };
        ret = spi_bus_add_device(SPI3_HOST, &devcfg, &s_lora_spi);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* SetStandby STDBY_RC (0x80) */
    uint8_t standby_rc = 0x00;
    if (lora_write_cmd(0x80, &standby_rc, 1) != ESP_OK) {
        ESP_LOGW(TAG, "SX1262 SetStandby failed - radio disabled");
        gpio_set_level(BOARD_PIN_LORA_NSS, 0);
        gpio_set_level(BOARD_PIN_LORA_RST, 0);
        gpio_set_level(BOARD_PIN_LORA_EN, 0);
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.enabled = true;
            s_stats.initialized = false;
            xSemaphoreGive(s_stats_mutex);
        }
        return ESP_OK;
    }

    uint8_t reg_mode = 0x01; /* DC-DC mode (0x96) */
    lora_write_cmd(0x96, &reg_mode, 1);

    uint8_t dio2_rf_sw = 0x01; /* DIO2 as RF switch (0x9D) */
    lora_write_cmd(0x9D, &dio2_rf_sw, 1);

    /* TCXO 1.8V, ~5ms delay: 0x02, 0x00, 0x01, 0x40 (0x97) */
    uint8_t tcxo_cfg[4] = { 0x02, 0x00, 0x01, 0x40 };
    lora_write_cmd(0x97, tcxo_cfg, 4);

    /* Calibrate all blocks (0x89) */
    uint8_t cal_all = 0x7F;
    lora_write_cmd(0x89, &cal_all, 1);

    /* Calibrate image for band (0x98) */
    uint8_t cal_img[2];
    if (cfg->freq_khz >= 430000 && cfg->freq_khz <= 440000) {
        cal_img[0] = 0x6B; cal_img[1] = 0x6F;
    } else if (cfg->freq_khz >= 470000 && cfg->freq_khz <= 510000) {
        cal_img[0] = 0x75; cal_img[1] = 0x81;
    } else if (cfg->freq_khz >= 779000 && cfg->freq_khz <= 787000) {
        cal_img[0] = 0xC1; cal_img[1] = 0xC5;
    } else if (cfg->freq_khz >= 902000 && cfg->freq_khz <= 928000) {
        cal_img[0] = 0xE1; cal_img[1] = 0xE5;
    } else {
        cal_img[0] = 0xD7; cal_img[1] = 0xDB; /* Default 863-870 MHz */
    }
    lora_write_cmd(0x98, cal_img, 2);

    uint8_t pkt_type = 0x01; /* LoRa packet type (0x8A) */
    lora_write_cmd(0x8A, &pkt_type, 1);

    /* RF frequency (0x86) */
    uint32_t f_rf = (uint32_t)(((uint64_t)cfg->freq_khz * 1000ULL * (1ULL << 25)) / 32000000ULL);
    uint8_t freq[4] = {
        (uint8_t)((f_rf >> 24) & 0xFF),
        (uint8_t)((f_rf >> 16) & 0xFF),
        (uint8_t)((f_rf >> 8) & 0xFF),
        (uint8_t)(f_rf & 0xFF)
    };
    lora_write_cmd(0x86, freq, 4);

    /* Buffer base address: TX 0, RX 0 (0x8F) */
    uint8_t base_addr[2] = { 0x00, 0x00 };
    lora_write_cmd(0x8F, base_addr, 2);

    /* SX1262 PA Configuration (0x95): +22 dBm power amplifier */
    uint8_t pa_config[4] = { 0x04, 0x07, 0x00, 0x01 };
    lora_write_cmd(0x95, pa_config, 4);

    /* Over Current Protection (OCP): 140 mA limit at register 0x08E7 (0x38) */
    lora_write_reg(0x08E7, 0x38);

    /* TX Parameters (0x8E): power clamp [-9..22], ramp 200 us (0x04) */
    int8_t pwr = cfg->tx_power_dbm;
    if (pwr < -9) pwr = -9;
    if (pwr > 22) pwr = 22;
    uint8_t tx_params[2] = { (uint8_t)pwr, 0x04 };
    lora_write_cmd(0x8E, tx_params, 2);

    /* RX Boosted gain: register 0x08AC -> 0x96 for boosted, 0x94 for power saving */
    lora_write_reg(0x08AC, cfg->rx_boosted ? 0x96 : 0x94);

    /* Modulation Parameters (0x8B): SF, BW, CR, LDRO */
    uint8_t sf = cfg->sf >= 5 && cfg->sf <= 12 ? cfg->sf : 7;
    uint32_t bw_hz = cfg->bw_hz ? cfg->bw_hz : (uint32_t)(cfg->bw_khz ? cfg->bw_khz : 250) * 1000;
    uint8_t bw_reg = sx1262_bw_register(bw_hz);
    uint8_t cr_reg = cfg->cr >= 5 && cfg->cr <= 8 ? (cfg->cr - 4) : 1;

    /* LowDataRateOptimize: Symbol time Ts = 2^SF / BW > 16.38 ms */
    uint8_t ldro = (((uint64_t)(1u << sf) * 1000000ULL / bw_hz) > 16380ULL) ? 0x01 : 0x00;
    uint8_t mod_params[4] = { sf, bw_reg, cr_reg, ldro };
    lora_write_cmd(0x8B, mod_params, 4);

    /* SyncWord: registers 0x0740, 0x0741 via WriteRegister (0x0D) */
    uint16_t sw = cfg->sync_word ? cfg->sync_word : 0x1424;
    lora_write_reg(0x0740, (uint8_t)((sw >> 8) & 0xFF));
    lora_write_reg(0x0741, (uint8_t)(sw & 0xFF));

    /* Packet Parameters (0x8C): adaptive preamble, explicit header, payload 255, crc on, standard IQ */
    uint16_t preamble = (sf <= 8) ? 32 : 16;
    uint8_t pkt_params[6] = {
        (uint8_t)(preamble >> 8),
        (uint8_t)(preamble & 0xFF),
        0x00, /* Explicit header */
        0xFF, /* Max RX payload */
        0x01, /* CRC on */
        0x00  /* Standard IQ */
    };
    lora_write_cmd(0x8C, pkt_params, 6);

    /* SetDioIrqParams: RxDone(0x0002) | Timeout(0x0200) | CrcErr(0x0040) = 0x0242, DIO1 mask = 0x0002 (0x08) */
    uint8_t dio_irq[8] = { 0x02, 0x42, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
    lora_write_cmd(0x08, dio_irq, 8);

    /* Clear IRQ (0x02) */
    uint8_t clr_irq[2] = { 0x03, 0xFF };
    lora_write_cmd(0x02, clr_irq, 2);

    /* Arm continuous RX (0x82) */
    uint8_t rx_timeout[3] = { 0xFF, 0xFF, 0xFF };
    lora_write_cmd(0x82, rx_timeout, 3);

    /* Launch background receiver task if not created */
    if (s_task == NULL) {
        xTaskCreate(lora_rx_task, "lora_rx", 16384, NULL, 5, &s_task);
    }

    /* Configure and hook DIO1 interrupt */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_err));
    }
    gpio_isr_handler_remove(BOARD_PIN_LORA_DIO1);
    gpio_isr_handler_add(BOARD_PIN_LORA_DIO1, lora_dio1_isr_handler, s_task);
    gpio_intr_enable(BOARD_PIN_LORA_DIO1);

    if (gpio_get_level(BOARD_PIN_LORA_DIO1) == 1 && s_task != NULL) {
        xTaskNotifyGive(s_task);
    }

    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.initialized = true;
        s_stats.enabled = true;
        s_stats.freq_khz = cfg->freq_khz;
        s_stats.bw_hz = bw_hz;
        s_stats.bw_khz = (uint16_t)(bw_hz / 1000);
        s_stats.sf = sf;
        s_stats.cr = cfg->cr >= 5 && cfg->cr <= 8 ? cfg->cr : 5;
        s_stats.tx_power_dbm = pwr;
        s_stats.sync_word = sw;
        s_stats.rx_boosted = cfg->rx_boosted;
        s_stats.cad_enabled = cfg->cad_enabled;
        s_stats.client_repeat = cfg->client_repeat;
        s_stats.path_hash_mode = cfg->path_hash_mode;
        s_stats.airtime_factor_x10 = cfg->airtime_factor_x10;
        s_stats.wake_on_rx = cfg->wake_on_rx;
        s_stats.share_loc = cfg->share_loc;
        s_stats.ble_pin = active_pin;
        strncpy(s_stats.node_name, s_current_cfg.node_name, sizeof(s_stats.node_name) - 1);
        strncpy(s_stats.channel_name, s_current_cfg.channel_name, sizeof(s_stats.channel_name) - 1);
        xSemaphoreGive(s_stats_mutex);
    }

    meshcore_ble_init(s_current_cfg.node_name, s_stats.ble_pin, on_ble_rx);

    ESP_LOGI(TAG, "SX1262 MeshCore configured: %lu kHz, SF%u, BW %lu Hz, CR 4/%u, Pwr %d dBm, Sync 0x%04X, LDRO %u",
             (unsigned long)cfg->freq_khz, (unsigned)sf, (unsigned long)bw_hz,
             (unsigned)s_stats.cr, (int)pwr, (unsigned)sw, (unsigned)ldro);
    return ESP_OK;
}

esp_err_t lora_widget_init(void)
{
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
    esp_err_t err = lora_widget_apply(&def);
    if (err == ESP_OK) {
        meshcore_ble_init(def.node_name, s_stats.ble_pin, on_ble_rx);
    }
    return err;
}

void lora_widget_sleep(void)
{
    meshcore_ble_stop();
    if (s_stats.enabled && s_stats.wake_on_rx) {
        return;
    }
    gpio_intr_disable(BOARD_PIN_LORA_DIO1);
    if (s_stats.initialized && s_lora_spi != NULL) {
        uint8_t sleep_cfg = 0x00; /* cold start */
        lora_write_cmd(0x84, &sleep_cfg, 1);
    }
    gpio_set_level(BOARD_PIN_LORA_NSS, 0);
    gpio_set_level(BOARD_PIN_LORA_RST, 0);
    gpio_set_level(BOARD_PIN_LORA_EN, 0);

    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.initialized = false;
        xSemaphoreGive(s_stats_mutex);
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
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats.packet_count = 0;
        s_stats.packets_sent = 0;
        s_stats.recv_flood = 0;
        s_stats.recv_direct = 0;
        s_stats.sent_flood = 0;
        s_stats.sent_direct = 0;
        s_stats.recv_errors = 0;
        s_stats.tx_air_ms = 0;
        s_stats.rx_air_ms = 0;
        s_stats.last_rssi_dbm = 0;
        s_stats.last_snr_db = 0;
        s_stats.last_snr_q4 = 0;
        s_stats.last_packet_time_s = 0;
        s_stats.advert_count = 0;
        s_stats.neighbor_count = 0;
        s_stats.msg_count = 0;
        memset(s_stats.last_sender, 0, sizeof(s_stats.last_sender));
        memset(s_adverts, 0, sizeof(s_adverts));
        memset(s_neighbors, 0, sizeof(s_neighbors));
        memset(s_messages, 0, sizeof(s_messages));
        memset(s_seen_pkts, 0, sizeof(s_seen_pkts));
        s_seen_pkt_next = 0;
        xSemaphoreGive(s_stats_mutex);
    }
}

lora_stats_t lora_widget_get_stats(void)
{
    lora_stats_t copy = {0};
    if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_stats.ble_connected = meshcore_ble_is_connected();
        copy = s_stats;
        xSemaphoreGive(s_stats_mutex);
    }
    return copy;
}

void lora_widget_set_location(int32_t lat_e4, int32_t lon_e4)
{
    s_lat_e6 = lat_e4 * 100;
    s_lon_e6 = lon_e4 * 100;
    s_has_loc = (lat_e4 != 0 || lon_e4 != 0);
}

void lora_widget_set_battery(uint16_t batt_mv, uint8_t level_pct)
{
    s_batt_pct = (level_pct > 100) ? 100 : level_pct;
    if (batt_mv >= 2500 && batt_mv <= 4500) {
        s_batt_mv = batt_mv;
    } else {
        s_batt_mv = (uint16_t)(3000 + (uint16_t)s_batt_pct * 12);
    }
}

esp_err_t lora_widget_send_advert(bool flood)
{
    init_node_keys();

    uint8_t raw[256];
    size_t raw_sz = meshcore_build_advert(
        raw, sizeof(raw),
        flood,
        s_pub_key, s_prv_key,
        (uint32_t)time(NULL),
        s_current_cfg.node_name,
        MESHCORE_ADV_CHAT,
        s_current_cfg.share_loc && s_has_loc,
        s_lat_e6, s_lon_e6
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
        mark_packet_seen(&tx_pkt, now_ms);
    }

    esp_err_t err = sx1262_transmit(raw, raw_sz);
    if (err == ESP_OK) {
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (flood) {
                s_stats.sent_flood++;
            } else {
                s_stats.sent_direct++;
            }
            xSemaphoreGive(s_stats_mutex);
        }
    }
    return err;
}

esp_err_t lora_widget_send_discover(void)
{
    uint32_t tag = esp_random();
    uint8_t raw[64];
    size_t raw_sz = meshcore_build_discover_req(raw, sizeof(raw), tag);
    if (raw_sz == 0) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sx1262_transmit(raw, raw_sz);
    if (err == ESP_OK) {
        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.sent_direct++;
            xSemaphoreGive(s_stats_mutex);
        }
    }
    return err;
}

esp_err_t lora_widget_send_channel_msg_ts(uint32_t ts, const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[256];
    uint32_t msg_time = (ts != 0) ? ts : (uint32_t)time(NULL);
    size_t raw_sz = meshcore_build_grp_txt(
        raw, sizeof(raw),
        s_current_cfg.channel_psk,
        msg_time,
        s_current_cfg.node_name,
        text
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
        mark_packet_seen(&tx_pkt, now_ms);
    }

    esp_err_t err = sx1262_transmit(raw, raw_sz);
    if (err == ESP_OK) {
        meshcore_msg_t msg = {
            .timestamp = msg_time,
            .rx_time_s = (uint32_t)time(NULL),
            .rssi_dbm = 0,
            .snr_q4 = 0,
            .hops = 0,
            .outgoing = true,
        };
        snprintf(msg.text, sizeof(msg.text), "%s: %s", s_current_cfg.node_name, text);

        if (s_stats_mutex && xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats.sent_flood++;
            s_stats.last_packet_time_s = (uint32_t)time(NULL);
            strncpy(s_stats.last_sender, s_current_cfg.node_name, sizeof(s_stats.last_sender) - 1);
            s_stats.last_sender[sizeof(s_stats.last_sender) - 1] = '\0';
            append_message_history(&msg);
            xSemaphoreGive(s_stats_mutex);
        }
    }
    return err;
}

esp_err_t lora_widget_send_channel_msg(const char *text)
{
    return lora_widget_send_channel_msg_ts(0, text);
}

static bool parse_on_off(const char *s, bool default_val)
{
    if (!s) return default_val;
    while (*s == ' ' || *s == '\t') s++;
    if (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0) {
        return true;
    }
    if (strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0) {
        return false;
    }
    return default_val;
}

void lora_widget_exec_cli(const char *cmd, lora_config_t *cfg_inout, bool *cfg_changed, char *reply, size_t reply_sz)
{
    if (!reply || reply_sz == 0) {
        return;
    }
    reply[0] = '\0';
    if (cfg_changed) *cfg_changed = false;

    if (!cmd || cmd[0] == '\0') {
        snprintf(reply, reply_sz, "Empty command. Try 'help'.\n");
        return;
    }

    /* Trim leading whitespace */
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    if (*cmd == '\0') {
        snprintf(reply, reply_sz, "Empty command. Try 'help'.\n");
        return;
    }

    /* Standard single-word / exact-match MeshCore CLI commands */
    if (strcasecmp(cmd, "ver") == 0) {
        snprintf(reply, reply_sz, "MeshCore-NM-EPD-420-BW v1.18.0-mod-rev0.2\n");
        return;
    }
    if (strcasecmp(cmd, "board") == 0) {
        snprintf(reply, reply_sz, "RockBase NM-EPD-420 (ESP32-S3R8 / SX1262)\n");
        return;
    }
    if (strcasecmp(cmd, "clear stats") == 0) {
        lora_widget_reset_stats();
        snprintf(reply, reply_sz, "Stats cleared.\n");
        return;
    }
    if (strcasecmp(cmd, "advert.zerohop") == 0) {
        esp_err_t err = lora_widget_send_advert(false);
        snprintf(reply, reply_sz, "Advert zerohop broadcast: %s\n", esp_err_to_name(err));
        return;
    }
    if (strcasecmp(cmd, "discover.neighbors") == 0) {
        esp_err_t err = lora_widget_send_discover();
        snprintf(reply, reply_sz, "Discover request: %s\n", esp_err_to_name(err));
        return;
    }
    if (strcasecmp(cmd, "stats-core") == 0) {
        lora_stats_t st = lora_widget_get_stats();
        snprintf(reply, reply_sz,
                 "Core Stats:\n"
                 "  Board: RockBase NM-EPD-420 (ESP32-S3R8 / SX1262)\n"
                 "  Node: %s\n"
                 "  PubKey: %s\n"
                 "  Enabled: %s\n"
                 "  Initialized: %s\n",
                 s_current_cfg.node_name, st.pub_key_hex,
                 st.enabled ? "true" : "false", st.initialized ? "true" : "false");
        return;
    }
    if (strcasecmp(cmd, "stats-radio") == 0) {
        lora_stats_t st = lora_widget_get_stats();
        snprintf(reply, reply_sz,
                 "Radio Stats:\n"
                 "  Noise floor: %d dBm\n"
                 "  Last RSSI: %d dBm\n"
                 "  Last SNR: %.2f dB\n"
                 "  Airtime TX: %lu ms\n"
                 "  Airtime RX: %lu ms\n"
                 "  Receive errors: %lu\n",
                 st.noise_floor_dbm, st.last_rssi_dbm, (double)st.last_snr_q4 / 4.0,
                 (unsigned long)st.tx_air_ms, (unsigned long)st.rx_air_ms,
                 (unsigned long)st.recv_errors);
        return;
    }
    if (strcasecmp(cmd, "stats-packets") == 0) {
        lora_stats_t st = lora_widget_get_stats();
        snprintf(reply, reply_sz,
                 "Packet Stats:\n"
                 "  Packets RX: %lu (Flood: %lu, Direct: %lu)\n"
                 "  Packets TX: %lu (Flood: %lu, Direct: %lu)\n"
                 "  Errors: %lu\n",
                 (unsigned long)st.packet_count, (unsigned long)st.recv_flood, (unsigned long)st.recv_direct,
                 (unsigned long)st.packets_sent, (unsigned long)st.sent_flood, (unsigned long)st.sent_direct,
                 (unsigned long)st.recv_errors);
        return;
    }

    /* Handle 'get <param>' */
    if (strncasecmp(cmd, "get ", 4) == 0) {
        const char *p = cmd + 4;
        while (*p == ' ' || *p == '\t') p++;
        if (strcasecmp(p, "radio") == 0) {
            double f_mhz = (double)s_current_cfg.freq_khz / 1000.0;
            double bw_khz = (double)(s_current_cfg.bw_hz ? s_current_cfg.bw_hz : (uint32_t)s_current_cfg.bw_khz * 1000) / 1000.0;
            snprintf(reply, reply_sz, "%.3f,%.1f,%u,%u\n", f_mhz, bw_khz, s_current_cfg.sf, s_current_cfg.cr);
            return;
        }
        if (strcasecmp(p, "tx") == 0) {
            snprintf(reply, reply_sz, "%d\n", s_current_cfg.tx_power_dbm);
            return;
        }
        if (strcasecmp(p, "name") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.node_name);
            return;
        }
        if (strcasecmp(p, "radio.rxgain") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.rx_boosted ? "on" : "off");
            return;
        }
        if (strcasecmp(p, "cad") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.cad_enabled ? "on" : "off");
            return;
        }
        if (strcasecmp(p, "repeat") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.client_repeat ? "on" : "off");
            return;
        }
        if (strcasecmp(p, "af") == 0) {
            snprintf(reply, reply_sz, "%.1f\n", (double)s_current_cfg.airtime_factor_x10 / 10.0);
            return;
        }
        if (strcasecmp(p, "dutycycle") == 0) {
            unsigned int pct = (unsigned int)(1000 / (10 + s_current_cfg.airtime_factor_x10));
            snprintf(reply, reply_sz, "%u%%\n", pct);
            return;
        }
        if (strcasecmp(p, "path.hash.mode") == 0) {
            snprintf(reply, reply_sz, "%u\n", s_current_cfg.path_hash_mode);
            return;
        }
        if (strcasecmp(p, "freq") == 0) {
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)s_current_cfg.freq_khz);
            return;
        }
        if (strcasecmp(p, "bw") == 0) {
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)(s_current_cfg.bw_hz ? s_current_cfg.bw_hz : (uint32_t)s_current_cfg.bw_khz * 1000));
            return;
        }
        if (strcasecmp(p, "sf") == 0) {
            snprintf(reply, reply_sz, "%u\n", s_current_cfg.sf);
            return;
        }
        if (strcasecmp(p, "cr") == 0) {
            snprintf(reply, reply_sz, "%u\n", s_current_cfg.cr);
            return;
        }
        if (strcasecmp(p, "channel") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.channel_name);
            return;
        }
        if (strcasecmp(p, "psk") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.channel_psk);
            return;
        }
        if (strcasecmp(p, "loc") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.share_loc ? "on" : "off");
            return;
        }
        if (strcasecmp(p, "public.key") == 0) {
            snprintf(reply, reply_sz, "%s\n", s_stats.pub_key_hex);
            return;
        }
        if (strcasecmp(p, "pin") == 0) {
            snprintf(reply, reply_sz, "%06lu\n", (unsigned long)s_stats.ble_pin);
            return;
        }
        snprintf(reply, reply_sz, "Unknown parameter 'get %s'\n", p);
        return;
    }

    /* Handle 'set <param> <val>' */
    if (strncasecmp(cmd, "set ", 4) == 0) {
        const char *p = cmd + 4;
        while (*p == ' ' || *p == '\t') p++;
        char sparam[32] = {0};
        char sval[128] = {0};
        int parsed = sscanf(p, "%31s %127[^\r\n]", sparam, sval);
        if (parsed <= 0) {
            snprintf(reply, reply_sz, "Usage: set <param> <value>\n");
            return;
        }
        if (strcasecmp(sparam, "radio") == 0) {
            float f_val = 0.0f, bw_val = 0.0f;
            unsigned int sf_val = 0, cr_val = 0;
            if (sscanf(sval, "%f,%f,%u,%u", &f_val, &bw_val, &sf_val, &cr_val) >= 4) {
                if (cfg_inout) {
                    cfg_inout->freq_khz = (f_val < 3000.0f) ? (uint32_t)(f_val * 1000.0f + 0.5f) : (uint32_t)f_val;
                    uint32_t hz = (bw_val < 1000.0f) ? (uint32_t)(bw_val * 1000.0f + 0.5f) : (uint32_t)bw_val;
                    cfg_inout->bw_hz = hz;
                    cfg_inout->bw_khz = (uint16_t)(hz / 1000);
                    cfg_inout->sf = (uint8_t)sf_val;
                    cfg_inout->cr = (uint8_t)cr_val;
                    s_current_cfg.freq_khz = cfg_inout->freq_khz;
                    s_current_cfg.bw_hz = cfg_inout->bw_hz;
                    s_current_cfg.bw_khz = cfg_inout->bw_khz;
                    s_current_cfg.sf = cfg_inout->sf;
                    s_current_cfg.cr = cfg_inout->cr;
                    s_stats.freq_khz = cfg_inout->freq_khz;
                    s_stats.bw_hz = cfg_inout->bw_hz;
                    s_stats.bw_khz = cfg_inout->bw_khz;
                    s_stats.sf = cfg_inout->sf;
                    s_stats.cr = cfg_inout->cr;
                    if (cfg_changed) *cfg_changed = true;
                }
                snprintf(reply, reply_sz, "Radio set: %lu kHz, %lu Hz, SF%u, CR4/%u\n",
                         (unsigned long)s_current_cfg.freq_khz,
                         (unsigned long)s_current_cfg.bw_hz,
                         s_current_cfg.sf, s_current_cfg.cr);
            } else {
                snprintf(reply, reply_sz, "Usage: set radio <freq>,<bw>,<sf>,<cr>\n");
            }
            return;
        }
        if (strcasecmp(sparam, "tx") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->tx_power_dbm = (int8_t)atoi(sval);
                s_current_cfg.tx_power_dbm = cfg_inout->tx_power_dbm;
                s_stats.tx_power_dbm = cfg_inout->tx_power_dbm;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%d\n", s_current_cfg.tx_power_dbm);
            return;
        }
        if (strcasecmp(sparam, "name") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                strncpy(cfg_inout->node_name, sval, sizeof(cfg_inout->node_name) - 1);
                cfg_inout->node_name[sizeof(cfg_inout->node_name) - 1] = '\0';
                strncpy(s_current_cfg.node_name, cfg_inout->node_name, sizeof(s_current_cfg.node_name) - 1);
                strncpy(s_stats.node_name, cfg_inout->node_name, sizeof(s_stats.node_name) - 1);
                meshcore_ble_update_name(cfg_inout->node_name);
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.node_name);
            return;
        }
        if (strcasecmp(sparam, "radio.rxgain") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->rx_boosted = parse_on_off(sval, cfg_inout->rx_boosted);
                s_current_cfg.rx_boosted = cfg_inout->rx_boosted;
                s_stats.rx_boosted = cfg_inout->rx_boosted;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.rx_boosted ? "on" : "off");
            return;
        }
        if (strcasecmp(sparam, "cad") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->cad_enabled = parse_on_off(sval, cfg_inout->cad_enabled);
                s_current_cfg.cad_enabled = cfg_inout->cad_enabled;
                s_stats.cad_enabled = cfg_inout->cad_enabled;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.cad_enabled ? "on" : "off");
            return;
        }
        if (strcasecmp(sparam, "repeat") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->client_repeat = parse_on_off(sval, cfg_inout->client_repeat);
                s_current_cfg.client_repeat = cfg_inout->client_repeat;
                s_stats.client_repeat = cfg_inout->client_repeat;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.client_repeat ? "on" : "off");
            return;
        }
        if (strcasecmp(sparam, "af") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                float af = (float)atof(sval);
                cfg_inout->airtime_factor_x10 = (uint8_t)(af * 10.0f + 0.5f);
                s_current_cfg.airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                s_stats.airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%.1f\n", (double)s_current_cfg.airtime_factor_x10 / 10.0);
            return;
        }
        if (strcasecmp(sparam, "dutycycle") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                int pct = atoi(sval);
                if (pct < 1) pct = 1;
                if (pct > 100) pct = 100;
                cfg_inout->airtime_factor_x10 = (uint8_t)((100 - pct) * 10 / pct);
                s_current_cfg.airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                s_stats.airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                if (cfg_changed) *cfg_changed = true;
            }
            unsigned int pct = (unsigned int)(1000 / (10 + s_current_cfg.airtime_factor_x10));
            snprintf(reply, reply_sz, "%u%%\n", pct);
            return;
        }
        if (strcasecmp(sparam, "path.hash.mode") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                int m = atoi(sval);
                if (m >= 0 && m <= 2) {
                    cfg_inout->path_hash_mode = (uint8_t)m;
                    s_current_cfg.path_hash_mode = cfg_inout->path_hash_mode;
                    s_stats.path_hash_mode = cfg_inout->path_hash_mode;
                    if (cfg_changed) *cfg_changed = true;
                }
            }
            snprintf(reply, reply_sz, "%u\n", s_current_cfg.path_hash_mode);
            return;
        }
        if (strcasecmp(sparam, "freq") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                float f = (float)atof(sval);
                cfg_inout->freq_khz = (f < 3000.0f) ? (uint32_t)(f * 1000.0f + 0.5f) : (uint32_t)f;
                s_current_cfg.freq_khz = cfg_inout->freq_khz;
                s_stats.freq_khz = cfg_inout->freq_khz;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)s_current_cfg.freq_khz);
            return;
        }
        if (strcasecmp(sparam, "bw") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                float bw = (float)atof(sval);
                uint32_t hz = (bw < 1000.0f) ? (uint32_t)(bw * 1000.0f + 0.5f) : (uint32_t)bw;
                cfg_inout->bw_hz = hz;
                cfg_inout->bw_khz = (uint16_t)(hz / 1000);
                s_current_cfg.bw_hz = hz;
                s_current_cfg.bw_khz = cfg_inout->bw_khz;
                s_stats.bw_hz = hz;
                s_stats.bw_khz = cfg_inout->bw_khz;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)s_current_cfg.bw_hz);
            return;
        }
        if (strcasecmp(sparam, "sf") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->sf = (uint8_t)atoi(sval);
                s_current_cfg.sf = cfg_inout->sf;
                s_stats.sf = cfg_inout->sf;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%u\n", s_current_cfg.sf);
            return;
        }
        if (strcasecmp(sparam, "cr") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->cr = (uint8_t)atoi(sval);
                s_current_cfg.cr = cfg_inout->cr;
                s_stats.cr = cfg_inout->cr;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%u\n", s_current_cfg.cr);
            return;
        }
        if (strcasecmp(sparam, "channel") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                strncpy(cfg_inout->channel_name, sval, sizeof(cfg_inout->channel_name) - 1);
                cfg_inout->channel_name[sizeof(cfg_inout->channel_name) - 1] = '\0';
                strncpy(s_current_cfg.channel_name, cfg_inout->channel_name, sizeof(s_current_cfg.channel_name) - 1);
                strncpy(s_stats.channel_name, cfg_inout->channel_name, sizeof(s_stats.channel_name) - 1);
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.channel_name);
            return;
        }
        if (strcasecmp(sparam, "psk") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                strncpy(cfg_inout->channel_psk, sval, sizeof(cfg_inout->channel_psk) - 1);
                cfg_inout->channel_psk[sizeof(cfg_inout->channel_psk) - 1] = '\0';
                strncpy(s_current_cfg.channel_psk, cfg_inout->channel_psk, sizeof(s_current_cfg.channel_psk) - 1);
                uint8_t secret32[32];
                uint8_t ch_hash = 0;
                if (meshcore_channel_key_from_psk(s_current_cfg.channel_psk, secret32, &ch_hash)) {
                    s_stats.channel_hash = ch_hash;
                }
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.channel_psk);
            return;
        }
        if (strcasecmp(sparam, "loc") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->share_loc = parse_on_off(sval, cfg_inout->share_loc);
                s_current_cfg.share_loc = cfg_inout->share_loc;
                s_stats.share_loc = cfg_inout->share_loc;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", s_current_cfg.share_loc ? "on" : "off");
            return;
        }
        if (strcasecmp(sparam, "pin") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                uint32_t pin = (uint32_t)strtoul(sval, NULL, 10);
                if (pin >= 100000 && pin <= 999999) {
                    cfg_inout->ble_pin = pin;
                    s_current_cfg.ble_pin = pin;
                    s_stats.ble_pin = pin;
                    meshcore_ble_update_pin(pin);
                    if (cfg_changed) *cfg_changed = true;
                    snprintf(reply, reply_sz, "%06lu\n", (unsigned long)pin);
                } else {
                    snprintf(reply, reply_sz, "Error: PIN must be 100000..999999\n");
                }
            } else {
                snprintf(reply, reply_sz, "Usage: set pin <100000..999999>\n");
            }
            return;
        }
        snprintf(reply, reply_sz, "Unknown parameter 'set %s'\n", sparam);
        return;
    }

    char op[32] = {0};
    char arg[128] = {0};
    int matched = sscanf(cmd, "%31s %127[^\r\n]", op, arg);

    if (matched <= 0) {
        snprintf(reply, reply_sz, "Invalid command.\n");
        return;
    }

    if (strcasecmp(op, "help") == 0) {
        snprintf(reply, reply_sz,
                 "MeshCore CLI Commands:\n"
                 "  ver                  - Get firmware version\n"
                 "  board                - Get hardware board name\n"
                 "  get/set radio        - Get or set freq,bw,sf,cr\n"
                 "  get/set tx           - Get or set TX power (dBm)\n"
                 "  get/set name         - Get or set node name\n"
                 "  get/set radio.rxgain - Get or set RX boosted gain (on/off)\n"
                 "  get/set cad          - Get or set CAD channel activity detection (on/off)\n"
                 "  get/set repeat       - Get or set client repeat mode (on/off)\n"
                 "  get/set af           - Get or set airtime factor\n"
                 "  get/set dutycycle    - Get or set duty cycle limit (%%)\n"
                 "  get/set path.hash.mode - Get or set path hash mode (0|1|2)\n"
                 "  get/set pin          - Get or set BLE companion 6-digit PIN\n"
                 "  stats-core           - Core node metrics\n"
                 "  stats-radio          - Radio layer metrics\n"
                 "  stats-packets        - Packet counters\n"
                 "  clear stats          - Reset statistics counters\n"
                 "  advert / adv         - Broadcast node advert (flood)\n"
                 "  advert.zerohop       - Broadcast zero-hop advert\n"
                 "  discover.neighbors   - Broadcast zero-hop discover request\n"
                 "  neighbors            - List direct zero-hop neighbors\n"
                 "  nodes / peers        - List heard node adverts\n"
                 "  msg / send <text>    - Send group channel text message\n"
                 "  messages             - Show recent chat messages\n");
    } else if (strcasecmp(op, "adv") == 0 || strcasecmp(op, "advert") == 0) {
        esp_err_t err = lora_widget_send_advert(true);
        snprintf(reply, reply_sz, "Advert broadcast: %s\n", esp_err_to_name(err));
    } else if (strcasecmp(op, "disc") == 0 || strcasecmp(op, "discover") == 0) {
        esp_err_t err = lora_widget_send_discover();
        snprintf(reply, reply_sz, "Discover request: %s\n", esp_err_to_name(err));
    } else if (strcasecmp(op, "msg") == 0 || strcasecmp(op, "send") == 0) {
        if (arg[0] == '\0') {
            snprintf(reply, reply_sz, "Usage: msg <text>\n");
        } else {
            esp_err_t err = lora_widget_send_channel_msg(arg);
            snprintf(reply, reply_sz, "Message send: %s\n", esp_err_to_name(err));
        }
    } else if (strcasecmp(op, "node") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            strncpy(cfg_inout->node_name, arg, sizeof(cfg_inout->node_name) - 1);
            cfg_inout->node_name[sizeof(cfg_inout->node_name) - 1] = '\0';
            strncpy(s_current_cfg.node_name, cfg_inout->node_name, sizeof(s_current_cfg.node_name) - 1);
            strncpy(s_stats.node_name, cfg_inout->node_name, sizeof(s_stats.node_name) - 1);
            meshcore_ble_update_name(cfg_inout->node_name);
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Node name: %s\nPubKey: %.16s...\n",
                 s_current_cfg.node_name, s_stats.pub_key_hex);
    } else if (strcasecmp(op, "channel") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            strncpy(cfg_inout->channel_name, arg, sizeof(cfg_inout->channel_name) - 1);
            cfg_inout->channel_name[sizeof(cfg_inout->channel_name) - 1] = '\0';
            strncpy(s_current_cfg.channel_name, cfg_inout->channel_name, sizeof(s_current_cfg.channel_name) - 1);
            strncpy(s_stats.channel_name, cfg_inout->channel_name, sizeof(s_stats.channel_name) - 1);
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Channel: %s (hash 0x%02X)\n",
                 s_current_cfg.channel_name, s_stats.channel_hash);
    } else if (strcasecmp(op, "psk") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            strncpy(cfg_inout->channel_psk, arg, sizeof(cfg_inout->channel_psk) - 1);
            cfg_inout->channel_psk[sizeof(cfg_inout->channel_psk) - 1] = '\0';
            strncpy(s_current_cfg.channel_psk, cfg_inout->channel_psk, sizeof(s_current_cfg.channel_psk) - 1);
            uint8_t secret32[32];
            uint8_t ch_hash = 0;
            if (meshcore_channel_key_from_psk(s_current_cfg.channel_psk, secret32, &ch_hash)) {
                s_stats.channel_hash = ch_hash;
            }
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Channel PSK: %s (hash 0x%02X)\n",
                 s_current_cfg.channel_psk, s_stats.channel_hash);
    } else if (strcasecmp(op, "repeat") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->client_repeat = parse_on_off(arg, cfg_inout->client_repeat);
            s_current_cfg.client_repeat = cfg_inout->client_repeat;
            s_stats.client_repeat = cfg_inout->client_repeat;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Client repeat: %s\n", s_current_cfg.client_repeat ? "ON" : "OFF");
    } else if (strcasecmp(op, "loc") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->share_loc = parse_on_off(arg, cfg_inout->share_loc);
            s_current_cfg.share_loc = cfg_inout->share_loc;
            s_stats.share_loc = cfg_inout->share_loc;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Share location: %s\n", s_current_cfg.share_loc ? "ON" : "OFF");
    } else if (strcasecmp(op, "boost") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->rx_boosted = parse_on_off(arg, cfg_inout->rx_boosted);
            s_current_cfg.rx_boosted = cfg_inout->rx_boosted;
            s_stats.rx_boosted = cfg_inout->rx_boosted;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "RX Boosted: %s\n", s_current_cfg.rx_boosted ? "ON" : "OFF");
    } else if (strcasecmp(op, "power") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->tx_power_dbm = (int8_t)atoi(arg);
            s_current_cfg.tx_power_dbm = cfg_inout->tx_power_dbm;
            s_stats.tx_power_dbm = cfg_inout->tx_power_dbm;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "TX Power: %d dBm\n", s_current_cfg.tx_power_dbm);
    } else if (strcasecmp(op, "sf") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->sf = (uint8_t)atoi(arg);
            s_current_cfg.sf = cfg_inout->sf;
            s_stats.sf = cfg_inout->sf;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "SF: %u\n", s_current_cfg.sf);
    } else if (strcasecmp(op, "cr") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->cr = (uint8_t)atoi(arg);
            s_current_cfg.cr = cfg_inout->cr;
            s_stats.cr = cfg_inout->cr;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "CR: 4/%u\n", s_current_cfg.cr);
    } else if (strcasecmp(op, "freq") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            float f = (float)atof(arg);
            cfg_inout->freq_khz = (f < 3000.0f) ? (uint32_t)(f * 1000.0f + 0.5f) : (uint32_t)f;
            s_current_cfg.freq_khz = cfg_inout->freq_khz;
            s_stats.freq_khz = cfg_inout->freq_khz;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Frequency: %lu kHz\n", (unsigned long)s_current_cfg.freq_khz);
    } else if (strcasecmp(op, "bw") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            float bw = (float)atof(arg);
            uint32_t hz = (bw < 1000.0f) ? (uint32_t)(bw * 1000.0f + 0.5f) : (uint32_t)bw;
            cfg_inout->bw_hz = hz;
            cfg_inout->bw_khz = (uint16_t)(hz / 1000);
            s_current_cfg.bw_hz = hz;
            s_current_cfg.bw_khz = cfg_inout->bw_khz;
            s_stats.bw_hz = hz;
            s_stats.bw_khz = cfg_inout->bw_khz;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Bandwidth: %lu Hz (%u kHz)\n",
                 (unsigned long)s_current_cfg.bw_hz, s_current_cfg.bw_khz);
    } else if (strcasecmp(op, "stats") == 0 || strcasecmp(op, "status") == 0) {
        lora_stats_t st = lora_widget_get_stats();
        snprintf(reply, reply_sz,
                 "LoRa Statistics:\n"
                 "  Enabled: %s, Init: %s\n"
                 "  RX Packets: %lu (Flood: %lu, Direct: %lu, Errors: %lu)\n"
                 "  TX Packets: %lu (Flood: %lu, Direct: %lu)\n"
                 "  Signal: Last RSSI %d dBm, SNR %.2f dB, Noise %d dBm\n"
                 "  Airtime: TX %lu ms, RX %lu ms\n",
                 st.enabled ? "YES" : "NO", st.initialized ? "YES" : "NO",
                 (unsigned long)st.packet_count, (unsigned long)st.recv_flood,
                 (unsigned long)st.recv_direct, (unsigned long)st.recv_errors,
                 (unsigned long)st.packets_sent, (unsigned long)st.sent_flood,
                 (unsigned long)st.sent_direct,
                 st.last_rssi_dbm, (double)st.last_snr_q4 / 4.0, st.noise_floor_dbm,
                 (unsigned long)st.tx_air_ms, (unsigned long)st.rx_air_ms);
    } else if (strcasecmp(op, "nodes") == 0 || strcasecmp(op, "adverts") == 0 || strcasecmp(op, "peers") == 0) {
        lora_stats_t st = lora_widget_get_stats();
        size_t off = snprintf(reply, reply_sz, "Discovered Nodes (%u):\n", st.advert_count);
        for (int i = 0; i < st.advert_count && off < reply_sz - 64; i++) {
            off += snprintf(reply + off, reply_sz - off,
                            "  [%d] %-16s type %u, hops %u, RSSI %d dBm\n",
                            i + 1, s_adverts[i].name, s_adverts[i].node_type,
                            s_adverts[i].hops, s_adverts[i].rssi_dbm);
        }
    } else if (strcasecmp(op, "neighbors") == 0) {
        lora_stats_t st = lora_widget_get_stats();
        size_t off = snprintf(reply, reply_sz, "Direct Neighbors (%u):\n", st.neighbor_count);
        for (int i = 0; i < st.neighbor_count && off < reply_sz - 64; i++) {
            off += snprintf(reply + off, reply_sz - off,
                            "  [%d] prefix %02x%02x%02x%02x, RSSI %d dBm, SNR in %.2f dB\n",
                            i + 1,
                            s_neighbors[i].pub_key_prefix[0], s_neighbors[i].pub_key_prefix[1],
                            s_neighbors[i].pub_key_prefix[2], s_neighbors[i].pub_key_prefix[3],
                            s_neighbors[i].rssi_dbm, (double)s_neighbors[i].snr_in_q4 / 4.0);
        }
    } else if (strcasecmp(op, "messages") == 0) {
        lora_stats_t st = lora_widget_get_stats();
        size_t off = snprintf(reply, reply_sz, "Recent Messages (%u):\n", st.msg_count);
        for (int i = 0; i < st.msg_count && off < reply_sz - 128; i++) {
            off += snprintf(reply + off, reply_sz - off,
                            "  %s [%s]\n",
                            s_messages[i].text,
                            s_messages[i].outgoing ? "TX" : "RX");
        }
    } else {
        snprintf(reply, reply_sz, "Unknown command '%s'. Try 'help'.\n", op);
    }
}
