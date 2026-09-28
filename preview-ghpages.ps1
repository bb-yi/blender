param(
    [int]$Port = 8000,
    [switch]$SkipBuild,
    [string]$Python = "python"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = [IO.Path]::GetFullPath($PSScriptRoot)
$siteRoot = Join-Path $scriptDir "site"
$previewRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir ".preview_root"))
$previewSiteRoot = [IO.Path]::GetFullPath((Join-Path $previewRoot "blender"))

function Write-Status {
    param([string]$Message)
    Write-Host "[preview] $Message" -ForegroundColor Cyan
}

if (-not $SkipBuild) {
    Write-Status "Building multilingual site..."
    & $Python (Join-Path $scriptDir "build_multilingual.py")
    if ($LASTEXITCODE -ne 0) {
        throw "Bilingual build failed with exit code ${LASTEXITCODE}; previous preview was not changed."
    }
}

# Validate both languages before touching the last usable preview, also with -SkipBuild.
foreach ($relativePath in @("index.html", "release.html", "en\index.html", "en\release.html")) {
    if (-not (Test-Path -LiteralPath (Join-Path $siteRoot $relativePath) -PathType Leaf)) {
        throw "Bilingual site is incomplete: missing site/$relativePath; previous preview was not changed."
    }
}

# Never recursively clean a redirected or unexpected destination.
if ($previewSiteRoot -ne (Join-Path (Join-Path $scriptDir ".preview_root") "blender")) {
    throw "Refusing to clean unexpected preview path: $previewSiteRoot"
}
foreach ($path in @($previewRoot, $previewSiteRoot)) {
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Preview path must be an ordinary directory: $path"
        }
    }
}
if (Test-Path -LiteralPath $previewSiteRoot) {
    Remove-Item -LiteralPath $previewSiteRoot -Recurse -Force
}

New-Item -ItemType Directory -Path $previewSiteRoot -Force | Out-Null
Get-ChildItem -LiteralPath $siteRoot -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $previewSiteRoot -Recurse -Force
}

Write-Status "Preview root prepared at $previewRoot"
Write-Status "Open http://127.0.0.1:$Port/blender/"
Write-Status "Press Ctrl+C to stop"

& $Python -m http.server $Port --bind 127.0.0.1 --directory $previewRoot
if ($LASTEXITCODE -ne 0) {
    throw "Preview server failed with exit code $LASTEXITCODE."
}
