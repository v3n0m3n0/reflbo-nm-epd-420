# Releasing reflbo-nm-epd-420

Every public release of **reflbo-nm-epd-420** follows these engineering, licensing, and verification rules.

## 1. Versioning Policy
- Independent Semantic Versioning (`vMAJOR.MINOR.PATCH`, e.g., `v1.0.0`).
- Update `version.txt`, `components/lora_widget/lora_widget.c` (`ver`), `components/lora_widget/meshcore_companion.c` (`CMD_DEVICE_QUERY`), and `test/host/test_meshcore_proto.c` together before building.
- Maintain clear attribution to upstream [`Vybo/reflbo`](https://github.com/Vybo/reflbo) (base commit `14593da`) and third-party libraries (`THIRD_PARTY.md`, `NOTICE`, `LICENSE` `GPL-3.0-only`).

## 2. Anonymity & Binary Cleanliness
- Public commits and tags use the maintainer identity (`v3n0m3n0 <v3n0m3n0@users.noreply.github.com>`).
- No local filesystem paths, developer usernames, internal issue/task codes, or Wi-Fi/API secrets may appear in source files, comments, release notes, or compiled `.bin` files (`-fmacro-prefix-map` / `-ffile-prefix-map` in `CMakeLists.txt`).
- Every `.bin` artifact in `release/` and `build/` must pass the automated path-leak scan with `0` hits before tagging.

## 3. Clean Build & Automated Verification
Run all verification suites on the exact release commit:
1. **Docker Firmware Build & Packaging:**
   ```powershell
   .\tools\build_release.ps1
   ```
   Builds cleanly inside `espressif/idf:release-v6.1`, verifies `build/flasher_args.json` offsets (`0x0`, `0x8000`, `0x19000`, `0x20000`), generates `release/reflbo-nm-epd-420-full-16MB-v<ver>.bin` and `release/reflbo-nm-epd-420-v<ver>.bin`, runs the binary path-leak scan, and writes `release/SHA256SUMS.txt`.
2. **Host C/C++ Unit & Golden Render Tests (Release build):**
   ```bash
   cmake -S test/host -B build_host -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build_host
   ctest --test-dir build_host --output-on-failure
   ```
3. **Web UI & Python Tooling Tests:**
   ```bash
   node --test test/web/*.mjs
   PYTHONPATH=tools python3 -m unittest discover -s tools/tests -v
   ```
4. **Physical Hardware Smoke Test (`COM` Port):**
   - Back up existing device NVS/partition state before switching firmwares.
   - Flash with `.\release\flash.ps1 -Port COM15` (preserving NVS/LittleFS; never `--erase-all` over configured devices; `--after watchdog-reset` on USB-Serial-JTAG).
   - Run `tools/test_com_focus_clock.py` and confirm `version`, `panel status` (`timeouts: 0`), `sensors` (AHT20), `sleep stats`, and `lora` checks pass cleanly.

## 4. Git Tagging & `release/` Directory Policy
- Keep **only** the latest release binaries, flash scripts (`flash.ps1`, `flash.bat`), `README.md`, and `SHA256SUMS.txt` tracked inside `release/` on `main`.
- Historical release binaries belong exclusively on the **GitHub Releases** page (attached to their annotated `vX.Y.Z` tag).
- Add a dedicated changelog document under `docs/releases/vX.Y.Z.md` and update `RELEASE_NOTES.md` and `README.md` (`Latest Release`).
- Create an annotated tag (`git tag -a vX.Y.Z -m "reflbo-nm-epd-420 vX.Y.Z"`), push `main` and the tag, and attach all files from `release/` (`*.bin`, `SHA256SUMS.txt`, `flash.ps1`, `flash.bat`) plus the markdown notes from `docs/releases/vX.Y.Z.md` to the GitHub Release (marked *Latest release*).
