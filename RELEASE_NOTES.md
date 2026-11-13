# reflbo-nm-epd-420 Release Notes

---

## Latest Release: v1.0.0 (`2026-11-13`)

**First public release of `reflbo-nm-epd-420`** — porting [`Vybo/reflbo`](https://github.com/Vybo/reflbo) (`14593da`, `spec r56`) to the **RockBase NM-EPD-420 (BW)** hardware platform with SSD1683 e-paper support, AHT20 climate sensing, SX1262 LoRa MeshCore integration, BLE Companion bridge, and Focus Super Power Mode.

For full release details and flash package contents, see [docs/releases/v1.0.0.md](docs/releases/v1.0.0.md) and [release/README.md](release/README.md).

### Summary of v1.0.0 Changes
- **RockBase NM-EPD-420 Board Support Package:** Authoritative GPIO mapping (`components/board/include/board_pins.h`), power rail sequencing (`TEMP_CTL`, `CODEC_EN`, `LORA_EN`, `PA_CTRL`, `ADC_EN`), ES8311 standby isolation, and I²C ESD leakage prevention before deep sleep.
- **SSD1683 4.2″ 400×300 E-Paper Driver:** Complete rewrite behind the `st7305` display abstraction featuring partial differential refresh (`0x26` OLD / `0x24` NEW RAM sync), automatic full-refresh anti-ghosting cycle every 20 partial updates, ambient temperature compensation via AHT20, and non-hanging BUSY timeout diagnostics (`panel status`).
- **SX1262 LoRa & MeshCore Companion Bridge:** Full SX1262 radio driver (`SPI3_HOST`), MeshCore flood/direct packet codec, Ed25519 / AES-128 channel crypto (`Public` & custom PSK channels), NimBLE Companion app GATT bridge, on-device LoRa dashboard cards, and USB CLI commands (`ver`, `lora ...`).
- **AHT20 Sensor & Battery Model:** `0xBE, 0x08, 0x00` AHT20 initialization and sleep handling, gated ADC divider sampling, and battery percentage/voltage curve modeling.
- **Power & Stability Improvements:** `CONFIG_FREERTOS_HZ=1000` tick accuracy, Octal PSRAM Light Sleep retention (`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n`, `TAGMEM=n`), internal RC calibration (`CONFIG_RTC_CLK_CAL_CYCLES=3000`), and **Focus Super Power Mode** (`power focus`) for minimal quiescent wake overhead when LoRa is disabled.
- **Reproducible Release Packaging & Tooling:** Deterministic `CMakeLists.txt` prefix-map scrubbing (zero build-host path leaks), `tools/build_release.ps1` Docker build + `0x0` full-flash merge + SHA-256 checksums, standalone `release/flash.ps1` and `release/flash.bat` (`--after watchdog-reset`, NVS/LittleFS preservation), and GitHub Actions CI (`ci.yml`).
