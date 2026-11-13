param(
    [switch]$Clean,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$projectDir = (Get-Item "$PSScriptRoot\..").FullName
Set-Location $projectDir

if (-not (Test-Path "version.txt")) {
    Write-Error "version.txt not found."
    exit 1
}
$ver = (Get-Content "version.txt" -Raw).Trim()
if ($ver -notmatch '^\d+\.\d+\.\d+(-[A-Za-z0-9.\-]+)?$') {
    Write-Error "Invalid version string in version.txt: '$ver'"
    exit 1
}
Write-Host "=== Packaging reflbo-nm-epd-420 v$ver ===" -ForegroundColor Cyan

if (-not $SkipBuild) {
    if ($Clean) {
        Write-Host "[*] Cleaning sdkconfig and build/ ..." -ForegroundColor Yellow
        Remove-Item -Force -ErrorAction SilentlyContinue "sdkconfig"
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "build"
    }

    $mountArg = "${projectDir}:/project"
    $buildCmd = "set -e; cd /project; rm -f sdkconfig; idf.py set-target esp32s3; idf.py build"
    Write-Host "[*] Running clean ESP-IDF build in espressif/idf:release-v6.1..." -ForegroundColor Yellow
    docker run --rm -v $mountArg -w /project espressif/idf:release-v6.1 bash -lc $buildCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Docker ESP-IDF build failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

$flasherJsonPath = Join-Path $projectDir "build\flasher_args.json"
if (-not (Test-Path $flasherJsonPath)) {
    Write-Error "build\flasher_args.json missing after build."
    exit 1
}
$flasher = Get-Content $flasherJsonPath -Raw | ConvertFrom-Json
$flashMode = $flasher.flash_settings.flash_mode
$flashSize = $flasher.flash_settings.flash_size
$flashFreq = $flasher.flash_settings.flash_freq

Write-Host "[*] flasher_args.json settings: mode=$flashMode, size=$flashSize, freq=$flashFreq" -ForegroundColor Gray

$releaseDir = Join-Path $projectDir "release"
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

# Keep ONLY latest release binaries in release/
Get-ChildItem -Path $releaseDir -Filter "*.bin" | Remove-Item -Force

$appOutName = "reflbo-nm-epd-420-v$ver.bin"
$fullOutName = "reflbo-nm-epd-420-full-16MB-v$ver.bin"

Copy-Item "build\bootloader\bootloader.bin" (Join-Path $releaseDir "bootloader.bin") -Force
Copy-Item "build\partition_table\partition-table.bin" (Join-Path $releaseDir "partition-table.bin") -Force
Copy-Item "build\ota_data_initial.bin" (Join-Path $releaseDir "ota_data_initial.bin") -Force
Copy-Item "build\reflbo-nm-epd-420.bin" (Join-Path $releaseDir $appOutName) -Force

Write-Host "[*] Merging unified 0x0 image -> release/$fullOutName ..." -ForegroundColor Yellow
$mountArg = "${projectDir}:/project"
$mergeCmd = "set -e; cd /project; python3 -m esptool --chip esp32s3 merge_bin -o release/$fullOutName --flash_mode $flashMode --flash_size $flashSize --flash_freq $flashFreq 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x19000 build/ota_data_initial.bin 0x20000 build/reflbo-nm-epd-420.bin"
docker run --rm -v $mountArg -w /project espressif/idf:release-v6.1 bash -lc $mergeCmd
if ($LASTEXITCODE -ne 0) {
    Write-Error "esptool merge_bin failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "[*] Scanning release/ and build/ binaries for local path leaks..." -ForegroundColor Yellow
$scanPy = @'
import glob, os, re, sys

patterns = [
    rb"[A-Za-z]:[/\\]Users[/\\][A-Za-z0-9._-]+",
    rb"/mnt/[a-z]/Users/[A-Za-z0-9._-]+",
    rb"/Users/[A-Za-z0-9._-]+",
    rb"/home/runner",
]
files = sorted(glob.glob("release/*.bin")) + [
    "build/bootloader/bootloader.bin",
    "build/partition_table/partition-table.bin",
    "build/ota_data_initial.bin",
    "build/reflbo-nm-epd-420.bin",
]
leaks = 0
for path in files:
    if not os.path.exists(path):
        continue
    data = open(path, "rb").read()
    hits = [m.group(0).decode("ascii", "ignore") for pat in patterns for m in pat.finditer(data)]
    print(f"  {path}: {len(data)} B, leak_hits={len(hits)} {hits if hits else ''}")
    if hits:
        leaks += len(hits)
sys.exit(1 if leaks else 0)
'@

$scanResult = $scanPy | wsl python3 -
if ($LASTEXITCODE -ne 0) {
    $scanResult | Write-Host
    Write-Error "Binary path leak check FAILED!"
    exit 1
}
$scanResult | ForEach-Object { Write-Host $_ }

Write-Host "[*] Generating release/SHA256SUMS.txt ..." -ForegroundColor Yellow
Push-Location $releaseDir
try {
    $targets = Get-ChildItem -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | Sort-Object Name
    $lines = foreach ($f in $targets) {
        $hash = (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $($f.Name)"
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Join-Path $releaseDir "SHA256SUMS.txt"), (($lines -join "`n") + "`n"), $utf8NoBom)
} finally {
    Pop-Location
}

Write-Host "`n[OK] Release v$ver ready in release/:" -ForegroundColor Green
Get-Content (Join-Path $releaseDir "SHA256SUMS.txt")
