param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

# Find python with esptool
$python = "python"
if (Test-Path "$env:USERPROFILE\.platformio\penv\Scripts\python.exe") {
    $python = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
}

$esptool = "esptool.py"
if (Test-Path "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py") {
    $esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
}

$projectDir = (Get-Item "$PSScriptRoot\..").FullName
Set-Location $projectDir

Write-Host "Flashing reflbo-nm-epd-420 to $Port at $Baud baud..."

# Flashing command (--after watchdog-reset is REQUIRED to clear FORCE_DOWNLOAD_BOOT latch)
& $python -X utf8 $esptool --chip esp32s3 --port $Port --baud $Baud `
    --before default-reset --after watchdog-reset write-flash `
    --flash-mode dio --flash-size 16MB --flash-freq 80m `
    0x0 "build\bootloader\bootloader.bin" `
    0x8000 "build\partition_table\partition-table.bin" `
    0x19000 "build\ota_data_initial.bin" `
    0x20000 "build\reflbo-nm-epd-420.bin"

Write-Host "Flash complete. Run 'pio device monitor -b 115200 -p $Port' to view serial output."
