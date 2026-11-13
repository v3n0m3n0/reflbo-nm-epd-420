<#
.SYNOPSIS
    Flashes reflbo-nm-epd-420 prebuilt firmware to RockBase NM-EPD-420 (ESP32-S3).
.PARAMETER Port
    Target COM port (e.g. COM15). Auto-detects if omitted and a single port is present.
.PARAMETER FullEraseFlash
    If specified, writes the unified 0x0 full-16MB image (keeps or resets partitions only if you explicitly erase).
    By default, flashes bootloader, partition-table, ota_data_initial, and app while preserving NVS and LittleFS!
.PARAMETER Baud
    Serial baud rate (default: 921600).
#>
param(
    [string]$Port = "",
    [switch]$FullImage,
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host "   reflbo-nm-epd-420 (ESP32-S3) Release Flasher (PowerShell)  " -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan

if (-not $Port) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 1) {
        $Port = $ports[0]
        Write-Host "[*] Auto-detected COM port: $Port" -ForegroundColor Green
    } elseif ($ports.Count -gt 1) {
        Write-Host "Available COM ports:" -ForegroundColor Yellow
        for ($i = 0; $i -lt $ports.Count; $i++) {
            Write-Host "  [$i] $($ports[$i])"
        }
        $sel = Read-Host "Select index [0-$($ports.Count - 1)] or enter COM port"
        if ($sel -match '^\d+$' -and [int]$sel -lt $ports.Count) {
            $Port = $ports[[int]$sel]
        } else {
            $Port = $sel
        }
    } else {
        $Port = Read-Host "Enter COM port (e.g. COM15)"
    }
}

if (-not $Port) {
    Write-Error "No COM port provided."
    exit 1
}

$appBin = Get-ChildItem -Path $PSScriptRoot -Filter "reflbo-nm-epd-420-v*.bin" | Sort-Object Name -Descending | Select-Object -First 1
$fullBin = Get-ChildItem -Path $PSScriptRoot -Filter "reflbo-nm-epd-420-full-16MB-v*.bin" | Sort-Object Name -Descending | Select-Object -First 1

$pythonExe = "python"
if ($env:USERPROFILE -and (Test-Path "$env:USERPROFILE\.platformio\penv\Scripts\python.exe")) {
    $pythonExe = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
}

$esptoolArgs = @()
$srcEsptool = Get-ChildItem "$env:USERPROFILE\.platformio\packages\tool-esptoolpy@src-*\esptool.py" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($srcEsptool) {
    $esptoolArgs = @($srcEsptool.FullName)
} elseif ($env:USERPROFILE -and (Test-Path "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py")) {
    $esptoolArgs = @("$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py")
} else {
    $esptoolArgs = @("-m", "esptool")
}

$verOut = (& $pythonExe -X utf8 @esptoolArgs version 2>&1 | Out-String)
$isV5 = $verOut -match 'v?5\.\d+'
$hasWatchdogReset = ($verOut -match 'v?([5-9]\.|4\.([7-9]|\d{2,}))')
$beforeArg = if ($isV5) { "default-reset" } else { "default_reset" }
$afterArg = if ($hasWatchdogReset) { "watchdog-reset" } else { "hard_reset" }
$cmdVerb = if ($isV5) { "write-flash" } else { "write_flash" }
$fmFlag = if ($isV5) { "--flash-mode" } else { "--flash_mode" }
$fsFlag = if ($isV5) { "--flash-size" } else { "--flash_size" }
$ffFlag = if ($isV5) { "--flash-freq" } else { "--flash_freq" }
if ($afterArg -ne "watchdog-reset") {
    Write-Warning "Installed esptool lacks --after watchdog-reset; using hard_reset (power-cycle via PWR if leaving BOOT download mode)."
}

if ($FullImage) {
    if (-not $fullBin) {
        Write-Error "Unified full image reflbo-nm-epd-420-full-16MB-v*.bin not found in $PSScriptRoot"
        exit 1
    }
    Write-Host "[*] Flashing unified 0x0 image: $($fullBin.Name) to $Port (--after $afterArg)..." -ForegroundColor Yellow
    & $pythonExe -X utf8 @esptoolArgs --chip esp32s3 --port $Port --baud $Baud `
        --before $beforeArg --after $afterArg $cmdVerb `
        $fmFlag dio $fsFlag 16MB $ffFlag 80m `
        0x0 $fullBin.FullName
} else {
    if (-not $appBin) {
        Write-Error "Application image reflbo-nm-epd-420-v*.bin not found in $PSScriptRoot"
        exit 1
    }
    $bootBin = Join-Path $PSScriptRoot "bootloader.bin"
    $partBin = Join-Path $PSScriptRoot "partition-table.bin"
    $otaBin  = Join-Path $PSScriptRoot "ota_data_initial.bin"
    foreach ($f in @($bootBin, $partBin, $otaBin)) {
        if (-not (Test-Path $f)) {
            Write-Error "Missing required partition binary: $f"
            exit 1
        }
    }
    Write-Host "[*] Flashing $($appBin.Name) + boot/partitions/otadata to $Port (--after $afterArg, preserving NVS/LittleFS)..." -ForegroundColor Yellow
    & $pythonExe -X utf8 @esptoolArgs --chip esp32s3 --port $Port --baud $Baud `
        --before $beforeArg --after $afterArg $cmdVerb `
        $fmFlag dio $fsFlag 16MB $ffFlag 80m `
        0x0 $bootBin 0x8000 $partBin 0x19000 $otaBin 0x20000 $appBin.FullName
}

if ($LASTEXITCODE -ne 0) {
    Write-Error "Flashing failed (exit code $LASTEXITCODE). Check COM port or hold BOOT while resetting."
    exit $LASTEXITCODE
}

Write-Host "[OK] Flashed successfully on $Port!" -ForegroundColor Green
