param(
  [string]$Version = "0.1.4"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "strata-console.exe"
if (-not (Test-Path $exe)) {
  & (Join-Path $root "build.ps1")
}

$dist = Join-Path $root "dist"
$stageName = "strata-console-v$Version-win64"
$stage = Join-Path $dist $stageName
$zip = Join-Path $dist "$stageName.zip"

New-Item -ItemType Directory -Path $dist -Force | Out-Null
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $root "strata-console.exe") -Destination $stage
Copy-Item -LiteralPath (Join-Path $root "ui") -Destination $stage -Recurse
foreach ($name in @("auto_config_helper.py", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "start-daemon.bat", "stop-console.bat")) {
  $p = Join-Path $root $name
  if (Test-Path $p) { Copy-Item -LiteralPath $p -Destination $stage }
}

if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

Write-Host "package: $zip"
Write-Host "contents:"
Get-ChildItem -LiteralPath $stage | Select-Object Name


