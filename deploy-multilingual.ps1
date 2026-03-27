# PowerShell script to deploy multi-language MkDocs site to GitHub Pages
# 部署中英文双语网站到 GitHub Pages

param(
    [string]$CommitMessage = "Update: Bilingual Documentation Website"
)

$ErrorActionPreference = "Stop"

function Write-Status {
    param([string]$Message, [string]$Status = "INFO")
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Write-Host "[$timestamp] [$Status] $Message" -ForegroundColor Cyan
}

function Write-Success {
    param([string]$Message)
    Write-Host "✅ $Message" -ForegroundColor Green
}

function Write-Error-Custom {
    param([string]$Message)
    Write-Host "❌ $Message" -ForegroundColor Red
}

# Get repository info
$repoUrl = git config --get remote.origin.url
$currentBranch = git branch --show-current
$sourceBranch = "docs"
$ghPagesBranch = "gh-pages"

Write-Host @"
╔════════════════════════════════════════════════════════════╗
║    多语言文档部署脚本 / Multi-Language Deployment Script   ║
║              Bilingual MkDocs to GitHub Pages              ║
╚════════════════════════════════════════════════════════════╝
"@ -ForegroundColor Yellow

Write-Status "Repository: $repoUrl"
Write-Status "Current branch: $currentBranch"
Write-Status "Source branch: $sourceBranch"
Write-Status "GitHub Pages branch: $ghPagesBranch"

Write-Status "Step 1: Verifying site directory..."
if (-not (Test-Path "site")) {
    Write-Error-Custom "❌ site/ directory not found! Running build..."
    python build_multilingual.py
}

if (-not (Test-Path "site")) {
    Write-Error-Custom "Failed to build site directory"
    exit 1
}

Write-Success "site/ directory verified"

# Check for site/en directory
if (-not (Test-Path "site/en")) {
    Write-Error-Custom "⚠️  Warning: site/en/ not found. English version may not be deployed."
}

Write-Status "Step 2: Adding all files to git..."
git add -A

$status = git status --porcelain
if ($status) {
    Write-Status "Changes detected, committing..."
    git commit -m $CommitMessage
    Write-Success "Changes committed"
} else {
    Write-Status "No changes to commit"
}

Write-Status "Step 3: Preparing gh-pages branch..."

# Check if gh-pages branch exists
$branchExists = git branch -r | Select-String "origin/$ghPagesBranch"

if (-not $branchExists) {
    Write-Status "Creating $ghPagesBranch branch..."
    git checkout --orphan $ghPagesBranch
    git rm -rf .
    git commit --allow-empty -m "Initial commit for GitHub Pages"
    git checkout $sourceBranch
} else {
    Write-Status "$ghPagesBranch branch already exists"
}

Write-Status "Step 4: Deploying site to $ghPagesBranch..."

# Subtree push
git subtree push --prefix site origin $ghPagesBranch

if ($LASTEXITCODE -eq 0) {
    Write-Success "Successfully pushed to $ghPagesBranch"
} else {
    Write-Error-Custom "Failed to push subtree. Attempting alternative method..."
    
    # Alternative: Clone and update gh-pages
    $tempDir = "temp_gh_pages_$(Get-Random)"
    Write-Status "Using temporary directory: $tempDir"
    
    git clone --branch $ghPagesBranch --single-branch (git config --get remote.origin.url) $tempDir
    Copy-Item "site/*" "$tempDir/" -Recurse -Force
    
    Push-Location $tempDir
    git add -A
    git commit -m $CommitMessage
    git push origin $ghPagesBranch
    Pop-Location
    
    Remove-Item $tempDir -Recurse -Force
    Write-Success "Deployment completed with alternative method"
}

Write-Host @"
╔════════════════════════════════════════════════════════════╗
║                   部署完成！                              ║
║              Deployment Complete!                         ║
╚════════════════════════════════════════════════════════════╝

📍 中文版本 (Chinese): https://bb-yi.github.io/blender/
📍 英文版本 (English): https://bb-yi.github.io/blender/en/

语言切换按钮位于网站右上角，靠近深色/亮色模式切换按钮
Language switcher is located in the top-right corner
"@ -ForegroundColor Green

Write-Status "Pushing source branch to ensure docs are saved..."
git push origin $sourceBranch

Write-Success "All done! Your documentation is now live."
