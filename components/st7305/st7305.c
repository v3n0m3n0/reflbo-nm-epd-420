#include "st7305.h"

#include <stddef.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "util_ticks.h"

#define PIN_MOSI    BOARD_PIN_EPD_MOSI
#define PIN_SCLK    BOARD_PIN_EPD_SCK
#define PIN_DC      BOARD_PIN_EPD_DC
#define PIN_CS      BOARD_PIN_EPD_CS
#define PIN_RST     BOARD_PIN_EPD_RST
#define PIN_BUSY    BOARD_PIN_EPD_BUSY
#define SPI_HOST_ID SPI2_HOST

static const char *TAG = "ssd1683";

static spi_device_handle_t s_spi;
static bool s_bus_ready;
static uint8_t *s_panel;       /* DMA capable internal / SPIRAM staging buffer */
static uint8_t *s_prev_panel;  /* Previous panel buffer for differential partial refresh */
static st7305_variant_t s_variant = ST7305_VARIANT_FACTORY;
static bool s_asleep;
static bool s_power_on;
static uint32_t s_partial_count;
static RTC_DATA_ATTR uint32_t s_full_count;
static RTC_DATA_ATTR uint32_t s_busy_timeout_count;
static int16_t s_ambient_c = 25;
static const uint32_t k_max_consecutive_partials = 20;

static void set_cs(int level) { gpio_set_level(PIN_CS, level); }
static void set_dc(int level) { gpio_set_level(PIN_DC, level); }
static void set_rst(int level) { gpio_set_level(PIN_RST, level); }

static esp_err_t read_busy(const char *step, int timeout_ms)
{
    const int max_wait_ms = (timeout_ms > 0) ? timeout_ms : 8000;
    vTaskDelay(pdMS_TO_TICKS(10));
    int waited_ms = 0;
    while (gpio_get_level(PIN_BUSY) == 1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
        if (waited_ms >= max_wait_ms) {
            s_busy_timeout_count++;
            ESP_LOGE(TAG, "read_busy timeout (%d ms) at [%s]", max_wait_ms, step);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static void send_command(uint8_t cmd)
{
    set_dc(0);
    set_cs(0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
    set_cs(1);
    set_dc(1);
}

static void send_data(uint8_t data)
{
    set_dc(1);
    set_cs(0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
    };
    spi_device_polling_transmit(s_spi, &t);
    set_cs(1);
}

static void write_bytes(const uint8_t *buf, size_t len)
{
    if (len == 0 || buf == NULL) return;
    set_dc(1);
    set_cs(0);
    static const size_t k_chunk_bytes = 4096;
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset < k_chunk_bytes) ? (len - offset) : k_chunk_bytes;
        spi_transaction_t t = {
            .length = 8 * chunk,
            .tx_buffer = buf + offset,
        };
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write_bytes failed at offset %u: %s", (unsigned)offset, esp_err_to_name(err));
            break;
        }
        offset += chunk;
    }
    set_cs(1);
}

static void set_ram_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    // RAM entry mode: 0x03 = X inc, Y inc
    send_command(0x11);
    send_data(0x03);

    // RAM X start & end byte address
    send_command(0x44);
    send_data(x / 8);
    send_data((x + w - 1) / 8);

    // RAM Y start & end address (16-bit)
    send_command(0x45);
    send_data(y % 256);
    send_data(y / 256);
    send_data((y + h - 1) % 256);
    send_data((y + h - 1) / 256);

    // RAM counters
    send_command(0x4E);
    send_data(x / 8);

    send_command(0x4F);
    send_data(y % 256);
    send_data(y / 256);
}

static esp_err_t power_on(void)
{
    if (!s_power_on) {
        send_command(0x22);
        send_data(0xE0);
        send_command(0x20);
        ESP_RETURN_ON_ERROR(read_busy("power_on", 1000), TAG, "power_on busy");
        s_power_on = true;
    }
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    if (s_power_on) {
        send_command(0x22);
        send_data(0x83);
        send_command(0x20);
        esp_err_t err = read_busy("power_off", 1000);
        s_power_on = false;
        return err;
    }
    return ESP_OK;
}

static esp_err_t hw_reset(void)
{
    set_rst(1);
    vTaskDelay(pdMS_TO_TICKS(10));
    set_rst(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    set_rst(1);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(read_busy("hw_reset", 2000), TAG, "hw_reset busy");

    send_command(0x12); /* SW reset */
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(read_busy("sw_reset", 2000), TAG, "sw_reset busy");

    /* Gate output: 300 lines */
    send_command(0x01);
    send_data(0x2B);
    send_data(0x01);
    send_data(0x00);

    /* Border Waveform control */
    send_command(0x3C);
    send_data(0x01);

    /* Temperature sensor */
    send_command(0x18);
    send_data(0x80);

    set_ram_area(0, 0, ST7305_WIDTH, ST7305_HEIGHT);
    ESP_RETURN_ON_ERROR(read_busy("init_done", 1000), TAG, "init_done busy");

    s_asleep = false;
    s_power_on = false;
    return ESP_OK;
}

static esp_err_t bus_init(void)
{
    if (s_panel == NULL) {
        s_panel = heap_caps_malloc(ST7305_FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_panel == NULL) {
            s_panel = heap_caps_malloc(ST7305_FRAME_BYTES, MALLOC_CAP_DMA);
        }
        ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_NO_MEM, TAG, "panel buffer");
    }
    if (s_prev_panel == NULL) {
        s_prev_panel = heap_caps_malloc(ST7305_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_prev_panel == NULL) {
            s_prev_panel = heap_caps_malloc(ST7305_FRAME_BYTES, MALLOC_CAP_DEFAULT);
        }
        ESP_RETURN_ON_FALSE(s_prev_panel != NULL, ESP_ERR_NO_MEM, TAG, "prev panel buffer");
        memset(s_prev_panel, 0xFF, ST7305_FRAME_BYTES);
    }

    gpio_hold_dis(PIN_CS);
    gpio_hold_dis(PIN_DC);
    gpio_hold_dis(PIN_RST);
    gpio_hold_dis(PIN_BUSY);
    gpio_hold_dis(PIN_SCLK);
    gpio_hold_dis(PIN_MOSI);

    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_conf), TAG, "output gpio config");
    set_cs(1);
    set_dc(1);
    set_rst(1);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_conf), TAG, "busy gpio config");

    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST7305_FRAME_BYTES + 64,
    };
    esp_err_t ret = spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI_HOST_ID, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_bus_ready = true;
    return ESP_OK;
}

esp_err_t st7305_init(st7305_variant_t variant)
{
    ESP_RETURN_ON_ERROR(bus_init(), TAG, "SPI bus");
    s_variant = variant;
    return hw_reset();
}

esp_err_t st7305_init_warm(st7305_variant_t variant, bool asleep)
{
    ESP_RETURN_ON_ERROR(bus_init(), TAG, "SPI bus");
    s_variant = variant;
    s_asleep = asleep;
    return hw_reset(); /* Wake SSD1683 controller from Deep Sleep Mode 1 */
}

esp_err_t st7305_reinit(st7305_variant_t variant)
{
    s_variant = variant;
    return hw_reset();
}

esp_err_t st7305_push(const uint8_t *canonical)
{
    ESP_RETURN_ON_FALSE(s_bus_ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    st7305_frame_to_panel(canonical, s_panel);

    bool force_full = (s_partial_count == 0) || (s_partial_count >= k_max_consecutive_partials);

    if (force_full) {
        /* Full refresh sequence with synthetic temperature LUT (fast 1.2s update) */
        set_ram_area(0, 0, ST7305_WIDTH, ST7305_HEIGHT);

        send_command(0x24);
        write_bytes(s_panel, ST7305_FRAME_BYTES);

        send_command(0x26);
        write_bytes(s_panel, ST7305_FRAME_BYTES);

        send_command(0x21);
        send_data(0x40);
        send_data(0x00);

        send_command(0x1A);
        send_data(s_ambient_c >= 15 ? 0x64 : (uint8_t)(s_ambient_c > 0 ? s_ambient_c : 0));

        send_command(0x22);
        send_data(0xD7);

        send_command(0x20);
        esp_err_t err = read_busy("full_refresh", 8000);
        s_power_on = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "full_refresh timeout, resetting panel");
            (void)hw_reset();
            s_partial_count = 0;
            return err;
        }

        memcpy(s_prev_panel, s_panel, ST7305_FRAME_BYTES);
        s_partial_count = 1;
        s_full_count++;
    } else {
        /* Differential partial refresh: power_on first, write 0x26 OLD and 0x24 NEW */
        ESP_RETURN_ON_ERROR(power_on(), TAG, "power_on");

        set_ram_area(0, 0, ST7305_WIDTH, ST7305_HEIGHT);
        send_command(0x26);
        write_bytes(s_prev_panel, ST7305_FRAME_BYTES);

        set_ram_area(0, 0, ST7305_WIDTH, ST7305_HEIGHT);
        send_command(0x24);
        write_bytes(s_panel, ST7305_FRAME_BYTES);

        set_ram_area(0, 0, ST7305_WIDTH, ST7305_HEIGHT);
        send_command(0x21);
        send_data(0x00);
        send_data(0x00);

        send_command(0x22);
        send_data(0xFC);

        send_command(0x20);
        esp_err_t err = read_busy("part_refresh", 5000);
        s_power_on = true;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "part_refresh timeout, resetting panel");
            (void)hw_reset();
            s_partial_count = 0;
            return err;
        }

        /* Keep controller RAM in sync */
        set_ram_area(0, 0, ST7305_WIDTH, ST7305_HEIGHT);
        send_command(0x26);
        write_bytes(s_panel, ST7305_FRAME_BYTES);

        ESP_RETURN_ON_ERROR(power_off(), TAG, "power_off");

        memcpy(s_prev_panel, s_panel, ST7305_FRAME_BYTES);
        s_partial_count++;
    }

    return ESP_OK;
}

esp_err_t st7305_seed_previous(const uint8_t *canonical, size_t len, uint8_t partial_count)
{
    ESP_RETURN_ON_FALSE(s_bus_ready && s_prev_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(canonical != NULL && len == ST7305_FRAME_BYTES, ESP_ERR_INVALID_ARG, TAG, "bad frame");
    st7305_frame_to_panel(canonical, s_prev_panel);
    s_partial_count = (partial_count > 0) ? partial_count : 1;
    return ESP_OK;
}

uint8_t st7305_partial_count(void)
{
    return (uint8_t)s_partial_count;
}

uint32_t st7305_full_count(void)
{
    return s_full_count;
}

uint32_t st7305_busy_timeout_count(void)
{
    return s_busy_timeout_count;
}

void st7305_set_ambient_c(int16_t temp_c)
{
    if (temp_c < -20) temp_c = -20;
    if (temp_c > 60) temp_c = 60;
    s_ambient_c = temp_c;
}

esp_err_t st7305_prepare_deep_sleep(void)
{
    ESP_RETURN_ON_ERROR(power_off(), TAG, "power_off");
    send_command(0x10);
    send_data(0x01); /* Deep sleep mode 1 */

    s_asleep = true;
    return ESP_OK;
}

void st7305_cancel_deep_sleep(void)
{
    s_asleep = false;
}

esp_err_t st7305_sleep_in(void)
{
    if (s_asleep) return ESP_OK;
    ESP_RETURN_ON_ERROR(power_off(), TAG, "power_off");
    send_command(0x10);
    send_data(0x01);
    s_asleep = true;
    return ESP_OK;
}

esp_err_t st7305_sleep_out(void)
{
    if (!s_asleep) return ESP_OK;
    ESP_RETURN_ON_ERROR(hw_reset(), TAG, "hw_reset");
    s_asleep = false;
    return ESP_OK;
}

bool st7305_asleep(void)
{
    return s_asleep;
}

st7305_variant_t st7305_variant(void)
{
    return s_variant;
}
