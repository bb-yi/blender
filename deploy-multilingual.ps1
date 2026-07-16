param(
    [string]$CommitMessage = "Deploy Blender 5.2 LTS NPR documentation"
)

# Compatibility entry point. Deployment logic lives in deploy-to-github.ps1.
& (Join-Path $PSScriptRoot "deploy-to-github.ps1") -CommitMessage $CommitMessage
exit $LASTEXITCODE
