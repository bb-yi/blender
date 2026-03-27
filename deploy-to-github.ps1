#!/usr/bin/env powershell
# GitHub Pages Auto Deploy for Blender NPR Documentation
# Usage: .\deploy-to-github.ps1

param(
    [string]$CommitMessage = "Update: Documentation Website"
)

function WriteSection {
    param([string]$Title)
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host $Title -ForegroundColor Cyan
    Write-Host "========================================`n" -ForegroundColor Cyan
}

function WriteOk {
    param([string]$Message)
    Write-Host "[OK] $Message" -ForegroundColor Green
}

function WriteError {
    param([string]$Message)
    Write-Host "[ERROR] $Message" -ForegroundColor Red
}

function WriteWarn {
    param([string]$Message)
    Write-Host "[INFO] $Message" -ForegroundColor Yellow
}

# Step 1: Check prerequisites
WriteSection "STEP 1: Checking Prerequisites"

Write-Host "Checking Git..." -ForegroundColor White
try {
    git --version | Out-Null
    WriteOk "Git is installed"
} catch {
    WriteError "Git not found. Please install from https://git-scm.com/"
    exit 1
}

Write-Host "`nChecking MkDocs..." -ForegroundColor White
try {
    python -m mkdocs --version | Out-Null
    WriteOk "MkDocs is installed"
} catch {
    WriteWarn "Installing MkDocs and Material theme..."
    pip install mkdocs mkdocs-material
    WriteOk "MkDocs installed"
}

# Step 2: Initialize Git
WriteSection "STEP 2: Initializing Git Repository"

if (-not (Test-Path ".git")) {
    Write-Host "Creating new Git repository..." -ForegroundColor White
    git init
    WriteOk "Git repository initialized"
} else {
    WriteOk "Git repository already exists"
}

# Step 3: Configure remote
WriteSection "STEP 3: Configuring Remote Origin"

$remoteExists = git remote | Select-String "origin"
if (-not $remoteExists) {
    Write-Host "Adding remote origin..." -ForegroundColor White
    git remote add origin https://github.com/bb-yi/blender.git
    WriteOk "Remote added: https://github.com/bb-yi/blender.git"
} else {
    WriteOk "Remote origin already configured"
}

# Step 4: Check for changes
WriteSection "STEP 4: Checking Local Changes"

# First ensure we have a main branch
Write-Host "Ensuring main branch exists..." -ForegroundColor White
$branchExists = git branch | Select-String "main"
if (-not $branchExists) {
    Write-Host "Creating main branch..." -ForegroundColor White
    git checkout -b main 2>&1 | Out-Null
}

$status = git status --porcelain
if ($status) {
    Write-Host "Found changes to commit:" -ForegroundColor White
    Write-Host $status
    
    Write-Host "`nStaging all changes..." -ForegroundColor White
    git add .
    
    Write-Host "Committing with message: $CommitMessage" -ForegroundColor White
    git commit -m $CommitMessage
    WriteOk "Changes committed"
} else {
    Write-Host "No changes to commit, but creating initial commit if needed..." -ForegroundColor White
    # Check if there are any commits
    $hasCommits = git rev-parse HEAD 2>&1
    if ($LASTEXITCODE -ne 0) {
        # No commits yet, create initial commit
        Write-Host "Creating initial commit..." -ForegroundColor White
        git add .
        git commit -m "Initial commit: Blender 5.1 NPR Port Documentation" 2>&1 | Out-Null
        WriteOk "Initial commit created"
    } else {
        WriteOk "Repository already has commits"
    }
}

# Step 5: Deploy to GitHub Pages
WriteSection "STEP 5: Deploying to GitHub Pages"

Write-Host "Running: mkdocs gh-deploy" -ForegroundColor White
try {
    mkdocs gh-deploy
    WriteOk "Deployed to GitHub Pages successfully!"
} catch {
    WriteError "Deployment failed"
    Write-Host $_
    exit 1
}

# Step 6: Push main branch
WriteSection "STEP 6: Pushing Main Branch"

Write-Host "Pushing to origin/main..." -ForegroundColor White
$pushSuccess = $false

# Try main branch first
try {
    $output = git push -u origin main 2>&1
    if ($LASTEXITCODE -eq 0) {
        WriteOk "Main branch pushed"
        $pushSuccess = $true
    }
} catch {
    # Silently continue to try master
}

# If main failed, try master
if (-not $pushSuccess) {
    Write-Host "Trying master branch instead..." -ForegroundColor White
    try {
        $output = git push -u origin master 2>&1
        if ($LASTEXITCODE -eq 0) {
            WriteOk "Master branch pushed"
            $pushSuccess = $true
        }
    } catch {
        # Continue anyway
    }
}

if (-not $pushSuccess) {
    WriteWarn "Branch push skipped (may already exist or no network)"
}

# Success
WriteSection "DEPLOYMENT COMPLETE!"

Write-Host "Your documentation is now live at:" -ForegroundColor White
Write-Host "  https://bb-yi.github.io/blender/" -ForegroundColor Cyan

Write-Host "`nNotes:" -ForegroundColor White
Write-Host "  - Pages may take 1-2 minutes to appear" -ForegroundColor White
Write-Host "  - Clear browser cache if you see old content (Ctrl+Shift+Del)" -ForegroundColor White
Write-Host "  - Verify settings: https://github.com/bb-yi/blender/settings/pages" -ForegroundColor White

Write-Host "`nFor future updates:" -ForegroundColor White
Write-Host "  git add ." -ForegroundColor Gray
Write-Host "  git commit -m 'your message'" -ForegroundColor Gray
Write-Host "  mkdocs gh-deploy" -ForegroundColor Gray
Write-Host "`n"

Read-Host "Press Enter to exit"
