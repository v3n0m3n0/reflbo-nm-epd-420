#pragma once

/*
 * RockBase NM-EPD-420 Pin Map (Authoritative reference: src/bsp/nm_display_420/config.h)
 */

/* Buttons */
#define BOARD_PIN_BOOT       0   /* BOOT button: active low */
#define BOARD_PIN_KEY        45  /* USER button (acts as KEY in reflbo UI): active low */

/* I2C shared bus (AHT20 @ 0x38, ES8311 @ 0x18) */
#define BOARD_PIN_I2C_SDA    39
#define BOARD_PIN_I2C_SCL    38

/* Power gates & controls */
#define BOARD_PIN_TEMP_CTL   40  /* AHT20 power gate - MUST be HIGH during runtime */
#define BOARD_PIN_CODEC_EN   44  /* ES8311 power gate - MUST be HIGH during runtime */
#define BOARD_PIN_PA_CTRL    41  /* Speaker amplifier enable - active HIGH (keep LOW when silent) */
#define BOARD_PIN_ADC_EN     43  /* Battery ADC divider enable - active HIGH, 20ms settle time */
#define BOARD_PIN_LORA_EN    47  /* SX1262 power gate - active HIGH */

/* Battery ADC */
#define BOARD_PIN_BAT_ADC    3   /* ADC1_CH2 (divider ratio x2) */

/* E-Paper Display (SSD1683 / GYE042A87 400x300 SPI) */
#define BOARD_PIN_EPD_SCK    2
#define BOARD_PIN_EPD_MOSI   1
#define BOARD_PIN_EPD_CS     46
#define BOARD_PIN_EPD_DC     4
#define BOARD_PIN_EPD_RST    5
#define BOARD_PIN_EPD_BUSY   6

/* LoRa SX1262 SPI (HT-RA62) */
#define BOARD_PIN_LORA_NSS   8
#define BOARD_PIN_LORA_SCK   9
#define BOARD_PIN_LORA_MOSI  10
#define BOARD_PIN_LORA_MISO  11
#define BOARD_PIN_LORA_RST   12
#define BOARD_PIN_LORA_BUSY  13
#define BOARD_PIN_LORA_DIO1  14  /* Ext1 wakeup source */

/* MicroSD Card (TF Slot) */
#define BOARD_PIN_TF_CS      7

/* I2S Audio Bus (ES8311) */
#define BOARD_PIN_I2S_MCLK   21
#define BOARD_PIN_I2S_SCLK   15
#define BOARD_PIN_I2S_LRCK   17
#define BOARD_PIN_I2S_DOUT   18
#define BOARD_PIN_I2S_DIN    16
