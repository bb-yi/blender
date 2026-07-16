[CmdletBinding()]
param(
    [string]$CommitMessage = "Deploy Blender 5.2 LTS NPR documentation"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $scriptDir ".git") -PathType Container)) {
    throw "This script must run from the blender-npr-doc-site Git repository."
}

$branch = (git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0 -or $branch -ne "docs") {
    throw "Expected the docs branch, found '$branch'."
}

$status = @(git status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read Git working-tree status."
}
if ($status.Count -ne 0) {
    throw "Commit or discard source changes before deployment. The deployment must be traceable to a clean docs revision."
}

Write-Host "[deploy] Rebuilding the complete bilingual site in strict mode..." -ForegroundColor Cyan
Invoke-Checked -FilePath "python" -ArgumentList @((Join-Path $scriptDir "build_multilingual.py"))

$required = @(
    "site\index.html",
    "site\release.html",
    "site\en\index.html",
    "site\en\release.html"
)
foreach ($relativePath in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $scriptDir $relativePath) -PathType Leaf)) {
        throw "Bilingual build is incomplete: missing $relativePath"
    }
}

Write-Host "[deploy] Refreshing the gh-pages deployment base..." -ForegroundColor Cyan
Invoke-Checked -FilePath "git" -ArgumentList @("fetch", "origin", "gh-pages")

$ghPagesWorktree = @(git worktree list --porcelain | Select-String -SimpleMatch "branch refs/heads/gh-pages")
if ($LASTEXITCODE -ne 0) {
    throw "Unable to inspect Git worktrees."
}
if ($ghPagesWorktree.Count -ne 0) {
    throw "The local gh-pages branch is checked out in another worktree. Remove that worktree before deployment."
}

Invoke-Checked -FilePath "git" -ArgumentList @(
    "branch",
    "--force",
    "gh-pages",
    "origin/gh-pages"
)

Write-Host "[deploy] Publishing site/ to origin/gh-pages..." -ForegroundColor Cyan
Invoke-Checked -FilePath "python" -ArgumentList @(
    "-m",
    "ghp_import",
    "--no-jekyll",
    "--push",
    "--remote",
    "origin",
    "--branch",
    "gh-pages",
    "--message",
    $CommitMessage,
    (Join-Path $scriptDir "site")
)

Write-Host "[deploy] Chinese: https://bb-yi.github.io/blender/" -ForegroundColor Green
Write-Host "[deploy] English: https://bb-yi.github.io/blender/en/" -ForegroundColor Green
