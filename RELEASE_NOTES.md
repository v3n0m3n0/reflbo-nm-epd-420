# reflbo-nm-epd-420 Release Notes

---

## Latest Release: v1.1.0 (`2026-11-14`)

**Feature, responsiveness, and radar update for `reflbo-nm-epd-420`** — adding European DWD OpenData WMS precipitation radar, instant preset switching with auto-hiding toast, faster cold sync & early NTP application, top-left Wi-Fi status visibility, modular MeshCore/SX1262 architecture with persistent NVS identity & encrypted secrets, drift-compensated sleep timing, and LittleFS health recovery.

For full release details and flash package contents, see [docs/releases/v1.1.0.md](docs/releases/v1.1.0.md) and [release/README.md](release/README.md).

### Summary of v1.1.0 Changes
- **DWD OpenData WMS Radar & Fixed RainViewer Fallback:** Replaced the Czechia-only ČHMÚ composite with open German Weather Service ([`opendata.dwd.de`](https://opendata.dwd.de)) GeoServer WMS layers (`Radar_rv_product_1x1km_ger` 1×1 km 5-minute composite over Central Europe / Western Poland and `Icon-eu_reg00625_fd_sl_TOTPREC01H` across all of Europe) backed by 1/2/4-bit indexed palette PNG decoding, fixed RainViewer `Universal Blue` precipitation classification and bounding-box coverage checks for global fallback, added stale in-flight fetch guards, and triggered automatic immediate radar refreshes when radar location/zoom changes.
- **Fast Cold Boot Sync, Top-Left Wi-Fi Icon & Instant UI Response:** Removed the redundant blank EPD flush during cold `display_init()`, applied NTP time immediately as soon as the SNTP step finishes (restoring the `8 s` per-network STA join timeout), displayed the top-left Wi-Fi status indicator whenever STA IP connectivity is active (except `Focus` and Setup QR), switched `KEY` preset changes to `0 ms` latency (moving auto-cycle toggling to the Menu and Web UI) with a compact 1-frame `22 px` toast auto-hiding after ~2.5 s, and deferred Focus mode radio/Wi-Fi tear-down cleanly behind the target preset render.
- **LoRa MeshCore Modularization, Security & Sleep Policy:** Separated the pure-C `sx1262_radio` driver and `meshcore_node` protocol engine (`board_spi3_lock()` serialization with `TF_CS`/`LORA_NSS` pre-high), moved the node Ed25519 identity seed (`lora_seed`, `lora_pin`) and custom `channel_psk` (`lora_psk`) into NVS `secrets` with `POST /api/lora/rekey` & CLI `reset-identity`, added hybrid DIO1 wake-on-RX light-sleep policy with BLE pause/resume across wakes, and exposed new LoRa dashboard fields (`lora.nodes`, `lora.msgs`, `lora.rssi`, `lora.snr`, `lora.last_msg`).
- **Display, Timekeeping & Storage Hardening:** Added `ssd1683.h` / `sw_rtc.h` hardware abstraction headers, signed sub-zero SSD1683 temperature compensation (`ssd1683_encode_temp_reg()`), `1 ms`-threshold software drift sleep compensation (`timekeeping_sleep_us_until()`), and LittleFS write-probe corruption auto-recovery with the `storage [status|format]` console CLI.

---

## Previous Release: v1.0.0 (`2026-11-13`)

**First public release of `reflbo-nm-epd-420`** — porting [`Vybo/reflbo`](https://github.com/Vybo/reflbo) (`14593da`, `spec r56`) to the **RockBase NM-EPD-420 (BW)** hardware platform with SSD1683 e-paper support, AHT20 climate sensing, SX1262 LoRa MeshCore integration, BLE Companion bridge, and Focus Super Power Mode.

For full release details and binary archive, see [docs/releases/v1.0.0.md](docs/releases/v1.0.0.md).

### Summary of v1.0.0 Changes
- **RockBase NM-EPD-420 Board Support Package:** Authoritative GPIO mapping (`components/board/include/board_pins.h`), power rail sequencing (`TEMP_CTL`, `CODEC_EN`, `LORA_EN`, `PA_CTRL`, `ADC_EN`), ES8311 standby isolation, and I²C ESD leakage prevention before deep sleep.
- **SSD1683 4.2″ 400×300 E-Paper Driver:** Complete rewrite behind the `st7305` display abstraction featuring partial differential refresh (`0x26` OLD / `0x24` NEW RAM sync), automatic full-refresh anti-ghosting cycle every 20 partial updates, ambient temperature compensation via AHT20, and non-hanging BUSY timeout diagnostics (`panel status`).
- **SX1262 LoRa & MeshCore Companion Bridge:** Full SX1262 radio driver (`SPI3_HOST`), MeshCore flood/direct packet codec, Ed25519 / AES-128 channel crypto (`Public` & custom PSK channels), NimBLE Companion app GATT bridge, on-device LoRa dashboard cards, and USB CLI commands (`ver`, `lora ...`).
- **AHT20 Sensor & Battery Model:** `0xBE, 0x08, 0x00` AHT20 initialization and sleep handling, gated ADC divider sampling, and battery percentage/voltage curve modeling.
- **Power & Stability Improvements:** `CONFIG_FREERTOS_HZ=1000` tick accuracy, Octal PSRAM Light Sleep retention (`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n`, `TAGMEM=n`), internal RC calibration (`CONFIG_RTC_CLK_CAL_CYCLES=3000`), and **Focus Super Power Mode** (`power focus`) for minimal quiescent wake overhead when LoRa is disabled.
- **Reproducible Release Packaging & Tooling:** Deterministic `CMakeLists.txt` prefix-map scrubbing (zero build-host path leaks), `tools/build_release.ps1` Docker build + `0x0` full-flash merge + SHA-256 checksums, standalone `release/flash.ps1` and `release/flash.bat` (`--after watchdog-reset`, NVS/LittleFS preservation), and GitHub Actions CI (`ci.yml`).
