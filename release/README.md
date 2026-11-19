# Prebuilt Release Binaries (`v1.2.0`)

This directory tracks **only the current release** of `reflbo-nm-epd-420` (`v1.2.0`) together with its standalone flash scripts and SHA-256 verification sums. Older release archives are hosted on **GitHub Releases**.

## Files
- `reflbo-nm-epd-420-full-16MB-v1.2.0.bin` — Unified factory flash image starting at `0x0` (bootloader + partition table + initial OTA data + application).
- `reflbo-nm-epd-420-v1.2.0.bin` — Standalone application image (`0x20000` / Web UI **Firmware** OTA upload).
- `bootloader.bin` (`0x0`), `partition-table.bin` (`0x8000`), `ota_data_initial.bin` (`0x19000`) — Supporting system images used for NVS/LittleFS-preserving flashes.
- `flash.ps1` / `flash.bat` — Windows flash scripts (auto-detects single COM port, enables UTF-8, uses `--after watchdog-reset` when supported by `esptool`, preserves NVS `0x9000` and LittleFS `0x830000`).
- `SHA256SUMS.txt` — SHA-256 checksums for all release assets.

## Quick Flash (Windows)
```powershell
.\flash.ps1 -Port COM15
```
Pass `-FullImage` to write `reflbo-nm-epd-420-full-16MB-v1.2.0.bin` directly at `0x0`:
```powershell
.\flash.ps1 -Port COM15 -FullImage
```

## Verify Checksums
```powershell
Get-FileHash *.bin, flash.ps1, flash.bat, README.md | Format-Table Hash, Path
```
Compare against `SHA256SUMS.txt`.
