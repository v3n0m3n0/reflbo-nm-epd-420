#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns true if the SX1262 SPI device handle has been attached.
 * Caller should hold the radio mutex or stats lock as appropriate.
 */
bool sx1262_radio_is_ready(void);

/**
 * @brief Maps bandwidth in Hz to the SX1262 SetModulationParams register code.
 */
uint8_t sx1262_bw_register(uint32_t bw_hz);

/**
 * @brief Configures GPIOs, SPI3 bus, and SX1262 registers for LoRa RX,
 *        hooks the DIO1 interrupt to @p rx_task, and starts continuous RX.
 *
 * Caller MUST hold @c s_radio_mutex across this call.
 *
 * @param[in]  cfg        Target LoRa radio configuration.
 * @param[in]  rx_task    Task handle to notify from DIO1 ISR (must not be NULL for RX wakeups).
 * @param[out] out_bw_hz  Resolved bandwidth in Hz.
 * @param[out] out_sf     Clamped spreading factor (5..12).
 * @param[out] out_cr     Clamped coding rate denominator (5..8).
 * @param[out] out_pwr    Clamped TX power (-9..22 dBm).
 * @param[out] out_sw     Effective sync word.
 * @param[out] out_ldro   Computed LDRO flag (0 or 1).
 * @param[out] out_hw_ok  Set to true if SX1262 responded cleanly, false on BUSY/SetStandby timeout
 *                        (in which case radio pins are powered down and ESP_OK is returned).
 */
esp_err_t sx1262_radio_configure(const lora_config_t *cfg, TaskHandle_t rx_task,
                                 uint32_t *out_bw_hz, uint8_t *out_sf, uint8_t *out_cr,
                                 int8_t *out_pwr, uint16_t *out_sw, uint8_t *out_ldro,
                                 bool *out_hw_ok);

/**
 * @brief Disables DIO1 interrupt, optionally sends SX1262 SetSleep cold-start command,
 *        and drives NSS/RST/EN low.
 *
 * Caller MUST hold @c s_radio_mutex across this call.
 */
void sx1262_radio_power_off(TaskHandle_t rx_task, bool send_cold_sleep);

/**
 * @brief Transmits a LoRa frame via SX1262 and restores continuous RX mode.
 *
 * Caller MUST hold @c s_radio_mutex across this call.
 *
 * @param[in]  data         Packet bytes (1..255 bytes).
 * @param[in]  len          Packet length in bytes.
 * @param[in]  bw_hz        Active bandwidth in Hz (for CAD/airtime calculation).
 * @param[in]  sf           Active spreading factor (for CAD/preamble/airtime).
 * @param[in]  cr           Active coding rate denominator (for airtime).
 * @param[in]  cad_enabled  Perform Listen-Before-Talk hardware CAD before TX.
 * @param[out] out_air_ms   Calculated packet airtime in milliseconds.
 * @param[in]  rx_task      Background RX task handle (notified if DIO1 latches HIGH on RX restore).
 */
esp_err_t sx1262_radio_transmit(const uint8_t *data, size_t len,
                                uint32_t bw_hz, uint8_t sf, uint8_t cr,
                                bool cad_enabled,
                                uint32_t *out_air_ms, TaskHandle_t rx_task);

/**
 * @brief Reads instantaneous RSSI (dBm) from SX1262 GetRssiInst (0x15).
 *
 * Caller MUST hold @c s_radio_mutex across this call.
 */
bool sx1262_radio_read_inst_rssi(int16_t *out_inst_dbm);

/**
 * @brief Reads SX1262 IRQ status (0x12), fetches RX metadata/FIFO if RxDone is set,
 *        and clears handled IRQ flags.
 *
 * Caller MUST hold @c s_radio_mutex across this call.
 *
 * @param[out] out_irq         Raw 16-bit IRQ status register.
 * @param[out] rx_buf          Buffer of at least 256 bytes to receive payload.
 * @param[out] out_payload_len Payload length read from GetRxBufferStatus.
 * @param[out] out_rssi_dbm    Packet RSSI in dBm.
 * @param[out] out_snr_q4      Packet SNR in 0.25 dB units.
 * @param[out] out_read_ok     True if RxDone payload was fetched cleanly.
 * @return True if GetIrqStatus SPI command succeeded, false otherwise.
 */
bool sx1262_radio_poll_rx(uint16_t *out_irq, uint8_t *rx_buf,
                          uint8_t *out_payload_len, int16_t *out_rssi_dbm,
                          int8_t *out_snr_q4, bool *out_read_ok);

#ifdef __cplusplus
}
#endif
