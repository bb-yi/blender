$dir = "e:\blender_bulid_test\blender_npr_bulid\blender-5.1-npr-doc-site\docs"
$files = @("scene-extensions.md", "extended-nodes.md", "npr-workflow.md", "interface-guide.md")

foreach ($file in $files) {
    $path = Join-Path $dir $file
    if (Test-Path $path) {
        $content = Get-Content -Path $path -Raw -Encoding UTF8
        
        # Remove English sections
        $content = $content -replace '=== "English"\s*\n\n.*?(?====|$)', ''
        
        # Remove Chinese tab markers
        $content = $content -replace '=== "中文"\s*\n\n', ''
        
        # Clean extra blank lines
        $content = $content -replace '\n\n\n+', "`n`n"
        
        Set-Content -Path $path -Value $content -Encoding UTF8
        Write-Host "✓ $file"
    }
}

Write-Host "`n✅ All files converted!"
