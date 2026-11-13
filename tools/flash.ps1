param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$python = "python"
if (Test-Path "$env:USERPROFILE\.platformio\penv\Scripts\python.exe") {
    $python = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
}

$esptoolArgs = @()
$srcEsptool = Get-ChildItem "$env:USERPROFILE\.platformio\packages\tool-esptoolpy@src-*\esptool.py" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($srcEsptool) {
    $esptoolArgs = @($srcEsptool.FullName)
} elseif (Test-Path "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py") {
    $esptoolArgs = @("$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py")
} else {
    $esptoolArgs = @("-m", "esptool")
}

$verOut = (& $python -X utf8 @esptoolArgs version 2>&1 | Out-String)
$isV5 = $verOut -match 'v?5\.\d+'
$hasWatchdogReset = ($verOut -match 'v?([5-9]\.|4\.([7-9]|\d{2,}))')
$beforeArg = if ($isV5) { "default-reset" } else { "default_reset" }
$afterArg = if ($hasWatchdogReset) { "watchdog-reset" } else { "hard_reset" }
$cmdVerb = if ($isV5) { "write-flash" } else { "write_flash" }
$fmFlag = if ($isV5) { "--flash-mode" } else { "--flash_mode" }
$fsFlag = if ($isV5) { "--flash-size" } else { "--flash_size" }
$ffFlag = if ($isV5) { "--flash-freq" } else { "--flash_freq" }
if (-not $hasWatchdogReset) {
    Write-Warning "Installed esptool lacks --after watchdog-reset; using hard_reset (power-cycle via PWR if exiting BOOT download mode)."
}

$projectDir = (Get-Item "$PSScriptRoot\..").FullName
Set-Location $projectDir

foreach ($req in @(
    "build\bootloader\bootloader.bin",
    "build\partition_table\partition-table.bin",
    "build\ota_data_initial.bin",
    "build\reflbo-nm-epd-420.bin"
)) {
    if (-not (Test-Path $req)) {
        Write-Error "Missing build artifact: $req. Run tools\build_release.ps1 or docker build first."
        exit 1
    }
}

Write-Host "Flashing reflbo-nm-epd-420 to $Port at $Baud baud (--after $afterArg, preserving NVS/storage)..."

& $python -X utf8 @esptoolArgs --chip esp32s3 --port $Port --baud $Baud `
    --before $beforeArg --after $afterArg $cmdVerb `
    $fmFlag dio $fsFlag 16MB $ffFlag 80m `
    0x0 "build\bootloader\bootloader.bin" `
    0x8000 "build\partition_table\partition-table.bin" `
    0x19000 "build\ota_data_initial.bin" `
    0x20000 "build\reflbo-nm-epd-420.bin"

if ($LASTEXITCODE -ne 0) {
    Write-Error "Flashing failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "Flash complete on $Port."
