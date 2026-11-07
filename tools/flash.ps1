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

& $python -X utf8 $esptool --chip esp32s3 --port $Port --baud $Baud `
    --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_size 16MB --flash_freq 80m `
    0x0 "build\bootloader\bootloader.bin" `
    0x8000 "build\partition_table\partition-table.bin" `
    0x19000 "build\ota_data_initial.bin" `
    0x20000 "build\reflbo-nm-epd-420.bin"

if ($LASTEXITCODE -ne 0) {
    Write-Error "Flashing failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "Flash complete. Run 'pio device monitor -b 115200 -p $Port' to view serial output."
