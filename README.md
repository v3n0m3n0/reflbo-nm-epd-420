# reflbo-nm-epd-420

**Ultra-low-power e-paper dashboard, LoRa MeshCore companion, and climate station firmware for the RockBase NM-EPD-420 (BW) (ESP32-S3R8 + SSD1683 4.2″ 400×300 E-Paper + SX1262 + AHT20).**

**Latest Release:** [`v1.0.0`](docs/releases/v1.0.0.md) · [`RELEASE_NOTES.md`](RELEASE_NOTES.md) · [`release/README.md`](release/README.md)

---

## Upstream Attribution & License

This project is an independent hardware port derived from Dan Vybiral's [**`Vybo/reflbo`**](https://github.com/Vybo/reflbo) at base commit [`14593da`](https://github.com/Vybo/reflbo/commit/14593da) (`2026-10-29`, `spec r56`).

- **License:** GNU General Public License, version 3 only (`GPL-3.0-only`). See [`LICENSE`](LICENSE), [`NOTICE`](NOTICE), and [`THIRD_PARTY.md`](THIRD_PARTY.md) (including the GPL §7 additional permission for Espressif ESP-IDF binary object libraries).
- **Added Third-Party Code:** LoRa packet/companion protocol and cryptography components in `components/lora_widget/` incorporate code adapted from **MeshCore** (`MIT`), **orlp/ed25519** (`zlib`), and **tiny-AES-c** (`Public domain / Unlicense`).
- **Corresponding Source:** Full source code for every release binary is published in this repository ([`https://github.com/v3n0m3n0/reflbo-nm-epd-420`](https://github.com/v3n0m3n0/reflbo-nm-epd-420)).

---

## Key Features

- **4.2″ SSD1683 B/W E-Paper Engine (`400×300`, 1 bpp):** Native SPI driver behind the `st7305` display abstraction featuring fast differential partial refreshes (`0x26` OLD / `0x24` NEW RAM synchronization), automatic full-refresh anti-ghosting sweep every 20 partial updates, AHT20 ambient temperature lookup, and BUSY timeout protection (`panel status`).
- **Watch-Face Dashboard & Custom Split Builder:** Fixed layouts (*Classic*, *Grid*, *Weather*, *Focus*, *Radar*, *Flights*, *Solar*, *Energy*) plus a user-configurable up-to-24-cell **Split** layout editor with live Web UI bitmap preview, custom slot titles, charts, bars, rings, gauges, and preset cycling schedules.
- **SX1262 LoRa & MeshCore Companion Bridge:** Full SX1262 transceiver driver on `SPI3_HOST` with gated `LORA_EN` power control, MeshCore flood/direct packet codec, Ed25519 identity & AES-128 channel crypto, NimBLE GATT companion bridge, on-device LoRa dashboard cards, and USB CLI (`lora [status|reset|on|off|advert|discover|send|cli]`, `ver`).
- **Focus Super Power Efficient Mode:** Switching to the **Focus clock** preset automatically activates Super Power Efficient mode—gating off LoRa wakeup locks and heavy background syncs, spacing AHT20 sampling to 15 minutes, updating the clock minute-by-minute via differential partial refreshes, and running periodic SNTP time/drift syncs.
- **Indoor Climate & Battery Gauge:** AHT20 temperature/humidity sensor (`0xBE, 0x08, 0x00` init + post-read sleep), configurable calibration offsets, gated battery ADC divider (`ADC_EN` pulsed per measurement), and custom/learned discharge curve tracking.
- **Connected Data Sources & Local Web UI:**
  - **Weather & Air:** Open-Meteo forecasts, UV, AQI, pollen, and local astronomical sunrise/sunset.
  - **Radars:** ČHMÚ / RainViewer precipitation radar and `adsb.fi` / `adsb.lol` live aircraft map.
  - **Solar & Energy:** Open-Meteo PV model, Forecast.Solar, Solcast, and SolaX Cloud (Developer API & Token ID).
  - **PC Telemetry:** WebSocket streaming from `reflbo-stats`.
  - **Device Web Configurator:** Responsive HTML/JS portal over SoftAP or STA (`reflbo-XXXX.local`), with OTA updates and JSON backup/restore.

---

## RockBase NM-EPD-420 Hardware Pinout

Authoritative pin definitions live in [`components/board/include/board_pins.h`](components/board/include/board_pins.h):

| Subsystem | Pin / Interface | GPIO | Runtime Notes |
|---|---|---|---|
| **E-Paper SSD1683 (400×300)** | SPI2 (`SCK` / `MOSI` / `CS` / `DC` / `RST` / `BUSY`) | `2` / `1` / `46` / `4` / `5` / `6` | Write-only panel; BUSY hardware polling |
| **LoRa SX1262 (HT-RA62)** | SPI3 (`NSS` / `SCK` / `MOSI` / `MISO`) | `8` / `9` / `10` / `11` | Dedicated SPI bus |
| **LoRa Control & IRQ** | `RST` / `BUSY` / `DIO1` / `LORA_EN` | `12` / `13` / `14` / `47` | `LORA_EN` active HIGH; `DIO1` ext1 wake capable |
| **Shared I²C Bus** | `SDA` / `SCL` (AHT20 `0x38`, ES8311 `0x18`) | `39` / `38` | `TEMP_CTL` & `CODEC_EN` stay HIGH while active |
| **Power Gates** | `TEMP_CTL` / `CODEC_EN` / `PA_CTRL` / `ADC_EN` | `40` / `44` / `41` / `43` | `PA_CTRL` kept LOW; `ADC_EN` HIGH only during reads |
| **Battery ADC** | `BAT_ADC` (`ADC1_CH2`, 100k/100k ×2) | `3` | Sampled asynchronously via `ADC_EN` gate |
| **Buttons** | `BOOT` / `KEY` (`USER`) | `0` / `45` | Active LOW with software debounce |
| **I²S Audio (ES8311)** | `MCLK` / `SCLK` / `LRCK` / `DOUT` / `DIN` | `21` / `15` / `17` / `18` / `16` | ES8311 placed in I²C standby at cold boot |
| **MicroSD (TF Slot)** | `TF_CS` | `7` | Chip select |

### Hardware & Firmware Design Safeguards
1. **1 kHz FreeRTOS Tick (`CONFIG_FREERTOS_HZ=1000`):** Prevents `pdMS_TO_TICKS(<10)` from rounding to zero ticks in display and I²C polling delays.
2. **SSD1683 Differential Partial Refresh Sequence:** Powers on the analog boost (`0x22`/`0xE0`, `0x20`, BUSY wait) before writing `0x26` (previous frame) and `0x24` (new frame), syncing both RAM banks cleanly.
3. **Octal PSRAM Light Sleep Retention:** Keeps CPU domain and tag memory powered (`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n`, `CONFIG_PM_POWER_DOWN_TAGMEM_IN_LIGHT_SLEEP=n`) to prevent Octal PSRAM cache corruption across light sleep wakes.
4. **Shared I²C ESD Diode Protection:** Holds `TEMP_CTL` (IO40) and `CODEC_EN` (IO44) HIGH whenever I²C is active, and cleanly resets I²C/I²S pullups in `board_prepare_deep_sleep()` before lowering power gates.
5. **AHT20 Calibration Command:** Initializes AHT20 with `0xBE, 0x08, 0x00` and puts the sensor to sleep after sampling.

---

## Flashing Prebuilt Release (`v1.0.0`)

Prebuilt binaries for the latest release live in [`release/`](release/) (see [`release/README.md`](release/README.md) for checksums and offsets). Historical release archives are published on **GitHub Releases**.

### Quick Flash on Windows (Preserves NVS & LittleFS Config)
```powershell
.\release\flash.ps1 -Port COM15
```
Or run `.\release\flash.bat COM15`.

### Unified Factory Flash (`0x0`, 16 MB QIO/DIO Image)
```sh
python -m esptool --chip esp32s3 --port COM15 --baud 921600 \
  --before default-reset --after watchdog-reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 release/reflbo-nm-epd-420-full-16MB-v1.0.0.bin
```

> **ESP32-S3 USB-Serial-JTAG Reset Note:** If the board entered ROM download mode via the `BOOT` button, a regular USB reset stays in download mode. The provided scripts automatically prefer `--after watchdog-reset` (requires `esptool >= v4.7`) so the chip re-samples strapping pins and boots the firmware immediately; otherwise, power-cycle using `PWR`.

---

## Building & Testing from Source

### 1. Firmware Build & Release Packaging (Docker ESP-IDF `release-v6.1`)
```powershell
# Interactive or one-shot build inside espressif/idf:release-v6.1
.\tools\docker-idf.ps1 "idf.py set-target esp32s3 && idf.py build"

# Full release pipeline: clean build + 0x0 merge + binary leak scan + release/ staging + SHA256SUMS.txt
.\tools\build_release.ps1
```
All build paths are scrubbed via `-fmacro-prefix-map` / `-ffile-prefix-map` in [`CMakeLists.txt`](CMakeLists.txt). Never edit `sdkconfig` directly—put defaults in [`sdkconfig.defaults`](sdkconfig.defaults) and local Wi-Fi overrides in the gitignored `sdkconfig.defaults.local`.

### 2. Host CTest, Web UI & Python Tooling Suites
```bash
# Host C/C++ unit tests & golden render checks (Release mode recommended for pixel-grid speed)
REFLBO_IDF_PATH=/opt/esp/idf cmake -S test/host -B build_host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_host
ctest --test-dir build_host --output-on-failure

# Web UI unit tests (no npm install needed)
node --test test/web/*.mjs

# Python host-tooling tests
PYTHONPATH=tools python3 -m unittest discover -s tools/tests -v
```

### 3. Hardware Verification Console (`115200` baud USB-Serial-JTAG)
Run automated Focus/display/sensor/RTC checks on a connected board (`COM15`):
```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\test_com_focus_clock.py --port COM15
```
Key console commands: `version`, `panel status`, `sensors`, `battery`, `rtc get`, `power stats`, `sleep stats`, `lora status`, `screenshot`, `preset list`.

---

## Documentation
- [**Release Notes (`RELEASE_NOTES.md`)**](RELEASE_NOTES.md) & [**`v1.0.0` Details**](docs/releases/v1.0.0.md)
- [**Release Policy (`RELEASE.md`)**](RELEASE.md)
- [**User Guide (`docs/guide.md`)**](docs/guide.md)
- [**Power Measurements (`docs/power.md`)**](docs/power.md)
- [**Developer & Architecture Guide (`AGENTS.md`)**](AGENTS.md)
