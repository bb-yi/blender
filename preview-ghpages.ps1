param(
    [int]$Port = 8000,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$previewRoot = Join-Path $scriptDir ".preview_root"
$previewSiteRoot = Join-Path $previewRoot "blender"

function Write-Status {
    param([string]$Message)
    Write-Host "[preview] $Message" -ForegroundColor Cyan
}

Set-Location $scriptDir

if (-not $SkipBuild) {
    Write-Status "Building multilingual site..."
    python build_multilingual.py
}

if (-not (Test-Path $previewRoot)) {
    New-Item -ItemType Directory -Path $previewRoot -Force | Out-Null
}

if (Test-Path $previewSiteRoot) {
    Remove-Item $previewSiteRoot -Recurse -Force
}

New-Item -ItemType Directory -Path $previewSiteRoot -Force | Out-Null
Copy-Item (Join-Path $scriptDir "site\*") $previewSiteRoot -Recurse -Force

Write-Status "Preview root prepared at $previewRoot"
Write-Status "Open http://127.0.0.1:$Port/blender/"
Write-Status "Press Ctrl+C to stop"

Set-Location $previewRoot
python -m http.server $Port