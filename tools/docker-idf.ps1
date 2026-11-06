# PowerShell helper to run idf commands inside the espressif/idf:release-v6.1 Docker container
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CommandArgs
)

$projectDir = (Get-Item "$PSScriptRoot\..").FullName
$cmd = if ($CommandArgs.Count -gt 0) { $CommandArgs -join " " } else { "idf.py build" }

Write-Host "Running in Docker [espressif/idf:release-v6.1]: $cmd"
docker run --rm -v "${projectDir}:/project" -w /project espressif/idf:release-v6.1 bash -c "$cmd"
