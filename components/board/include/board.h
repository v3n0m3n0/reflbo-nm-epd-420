#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * Board bring-up (spec §3.1). Owns the I²C bus and the GPIO ISR service. Call board_init() once,
 * first. `cold`: after power-on, also report the I²C devices and put the audio codecs into standby
 * (they keep that state through deep sleep).
 */
esp_err_t board_init(bool cold);
i2c_master_bus_handle_t board_i2c(void);
void board_prepare_deep_sleep(void);

/* Shared SPI3 bus (SX1262 LoRa + TF card CS deselect) with mutex serialization. */
esp_err_t board_spi3_init(void);
bool board_spi3_lock(uint32_t timeout_ms);
void board_spi3_unlock(void);
