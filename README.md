# reflbo-nm-epd-420

Port oprogramowania **[reflbo (Vybo)](https://github.com/Vybo/reflbo)** dla platformy sprzętowej **RockBase NM-EPD-420** (ESP32-S3R8, 4.2″ e-papier SSD1683, moduł radiowy SX1262 LoRa, kodek audio ES8311, czujnik klimatu AHT20).

Firmware zachowuje kompletny ekosystem dashboardów reflbo (zegar, kalendarz, klimat indoor, prognoza pogody Open-Meteo, radar deszczu, radar lotów ADSB, prognoza solarna, statystyki PC z HWiNFO, konfigurator Web), adaptując jednocześnie warstwę sprzętową do możliwości i specyfiki NM-EPD-420.

---

## 🛠️ Specyfikacja sprzętowa & Mapa pinów

Authoritative source: `components/board/include/board_pins.h` oraz `src/bsp/nm_display_420/config.h`.

| Peryferium | Pin ESP32-S3 | Funkcja / Rejestr | Uwagi |
|---|---|---|---|
| **E-Paper SCK** | GPIO2 | SPI2 Clock | Szyna SPI wyświetlacza |
| **E-Paper MOSI** | GPIO1 | SPI2 MOSI | Dane do SSD1683 |
| **E-Paper CS** | GPIO46 | Chip Select | Active LOW |
| **E-Paper DC** | GPIO4 | Data / Command | 0 = Command, 1 = Data |
| **E-Paper RST** | GPIO5 | Hardware Reset | Active LOW |
| **E-Paper BUSY** | GPIO6 | Busy Status | 1 = Busy, 0 = Ready |
| **I²C SDA** | GPIO39 | Magistrala I²C | AHT20 (0x38), ES8311 (0x18) |
| **I²C SCL** | GPIO38 | Magistrala I²C | Zewnętrzne rezystory pull-up 2.2kΩ |
| **TEMP_CTL** | GPIO40 | Zasilanie AHT20 | **Musi być stale HIGH w runtime** |
| **CODEC_EN** | GPIO44 | Zasilanie ES8311 | **Musi być stale HIGH w runtime** |
| **PA_CTRL** | GPIO41 | Wzmacniacz audio | Active HIGH (LOW = wyciszony, 0 mA) |
| **ADC_EN** | GPIO43 | Kluczowanie dzielnika baterii | HIGH = pomiar, LOW = eliminacja upływu prądu |
| **BATT_ADC** | GPIO3 | Wejście analogowe (ADC1_CH2) | Dzielnik napięcia ×2 |
| **LORA_EN** | GPIO47 | Zasilanie SX1262 | Active HIGH |
| **LORA_NSS** | GPIO8 | LoRa SPI Chip Select | SPI3_HOST |
| **LORA_SCK** | GPIO9 | LoRa SPI Clock | SPI3_HOST |
| **LORA_MOSI** | GPIO10 | LoRa SPI MOSI | SPI3_HOST |
| **LORA_MISO** | GPIO11 | LoRa SPI MISO | SPI3_HOST |
| **LORA_RST** | GPIO12 | LoRa Reset | Active LOW |
| **LORA_BUSY** | GPIO13 | LoRa Busy | 1 = Busy |
| **LORA_DIO1** | GPIO14 | LoRa IRQ / Ext1 Wakeup | Źródło wybudzania z deep sleep |
| **Przycisk USER** | GPIO45 | Przycisk nawigacji (KEY w reflbo) | Active LOW, pull-up włączony |
| **Przycisk BOOT** | GPIO0 | Przycisk BOOT / akcji | Active LOW |

---

## ⚡ Pułapki sprzętowe uwzględnione w implementacji

1. **FreeRTOS Tick Trap:** Wymuszone `CONFIG_FREERTOS_HZ=1000`. Przy domyślnym taktowaniu 100 Hz makro `pdMS_TO_TICKS(5)` ewaluuje do 0, co zamienia oczekiwanie na flagę BUSY w pętlę bez opóźnienia i przedwcześnie przerywa odświeżanie e-papieru.
2. **SSD1683 Partial Refresh Power-On:** Przed wysłaniem sekwencji różnicowego odświeżania częściowego (`0xFC`) włączana jest pompa napięć analogowych (`0x22` + `0xE0`, `0x20`, `read_busy`), a po odświeżeniu nowy bufor jest synchronizowany do banku pamięci `0x26`.
3. **Szybkie pełne odświeżanie:** Zastosowano syntetyczny profil temperaturowy LUT 90°C (`0x1A`, `0x64`, `0x22`, `0xD7`), redukujący czas pełnego odświeżenia do ok. 1.2 sekundy (zamiast standardowych 3–4 sekund).
4. **Współdzielona szyna I²C:** Piny `CODEC_EN` (GPIO44) i `TEMP_CTL` (GPIO40) są stale podciągnięte do stanu wysokiego podczas pracy. Stan niski na którymkolwiek z nich powodowałby upływ prądu przez diody ESD do masy i ściągnięcie SDA/SCL do 0V.
5. **Kalibracja AHT20:** Inicjalizacja AHT20 używa komendy `0xBE, 0x08, 0x00` (zamiast `0xE1` specyficznego dla AHT10).
6. **Kluczowanie ADC baterii:** Zasilanie dzielnika napięcia na GPIO43 jest włączane tylko na czas pomiaru (z 20 ms czasem stabilizacji), po czym natychmiast wyłączane, chroniąc ogniwo przed pasożytniczym rozładowaniem w czasie uśpienia.
7. **Pasywny monitor LoRa SX1262:** Zintegrowany widget nasłuchujący pakiety w standardzie MeshCore na paśmie EU868 (868.0 MHz, SF7, BW250kHz, CR4/5).

---

## 🚀 Budowanie i wgrywanie firmware

### 1. Kompilacja w środowisku Docker (ESP-IDF v6.1)

W katalogu projektu:
```powershell
.\tools\docker-idf.ps1
```
lub bezpośrednio:
```powershell
docker run --rm -v "${PWD}:/project" -w /project espressif/idf:release-v6.1 idf.py build
```

### 2. Wgrywanie na urządzenie (Windows)

```powershell
.\tools\flash.ps1 -Port COM12
```
*Uwaga: Skrypt automatycznie wymusza reset kontrolera przez watchdog (`--after watchdog-reset`), co zapobiega zablokowaniu ESP32-S3 w bootloaderze ROM.*

### 3. Monitorowanie portu szeregowego

```powershell
pio device monitor -b 115200 -p COM12
```

---

## 📄 Licencja

GPL-3.0-only. Wykorzystuje komponenty Open-Source zgodnie z [NOTICE](NOTICE) i [THIRD_PARTY.md](THIRD_PARTY.md).
