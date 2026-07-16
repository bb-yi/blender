@echo off
setlocal
cd /d "%~dp0"

echo.
echo ========================================
echo Blender 5.2 LTS NPR Port Documentation
echo ========================================
echo.

python --version >nul 2>&1
if errorlevel 1 (
    echo Error: Python is not available in PATH.
    exit /b 1
)

python -m mkdocs --version >nul 2>&1
if errorlevel 1 (
    echo Error: MkDocs is not installed.
    echo Run: python -m pip install mkdocs mkdocs-material
    exit /b 1
)

echo 1. Preview the bilingual GitHub Pages layout
echo 2. Build both languages in strict mode
echo 3. Serve the Chinese site only
echo 4. Serve the English site only
echo 5. Exit
echo.
set "choice=%~1"
if not defined choice set /p choice="Select an option (1-5): "

if "%choice%"=="1" powershell -ExecutionPolicy Bypass -File .\preview-ghpages.ps1
if "%choice%"=="2" python .\build_multilingual.py
if "%choice%"=="3" python -m mkdocs serve -f .\mkdocs.yml
if "%choice%"=="4" python -m mkdocs serve -f .\mkdocs.en.yml
if "%choice%"=="5" exit /b 0

if not "%choice%"=="1" if not "%choice%"=="2" if not "%choice%"=="3" if not "%choice%"=="4" if not "%choice%"=="5" (
    echo Invalid option.
    exit /b 1
)

exit /b %errorlevel%
