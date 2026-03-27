@echo off
REM Blender 5.1 NPR Port Documentation Website Quick Start

setlocal enabledelayedexpansion

echo.
echo ====================================
echo Blender 5.1 NPR Port Documentation  
echo ====================================
echo.

REM Check if Python is installed
python --version >nul 2>&1
if errorlevel 1 (
    echo Error: Python is not installed or not in PATH
    echo Please install Python first: https://www.python.org/
    pause
    exit /b 1
)

REM Check if mkdocs is installed
python -m mkdocs --version >nul 2>&1
if errorlevel 1 (
    echo.
    echo Installing MkDocs and Material theme...
    echo.
    pip install mkdocs mkdocs-material
    if errorlevel 1 (
        echo Error: Failed to install mkdocs
        pause
        exit /b 1
    )
)

echo.
echo 选择操作:
echo 1. 启动本地预览服务 (Ctrl+C 停止)
echo 2. 生成静态网站
echo 3. 退出
echo.

set /p choice="请选择 (1-3): "

if "%choice%"=="1" (
    echo.
    echo 启动本地服务器...
    echo 请在浏览器中打开: http://localhost:8000
    echo.
    python -m mkdocs serve
) else if "%choice%"=="2" (
    echo.
    echo 生成静态网站...
    echo.
    python -m mkdocs build
    if errorlevel 0 (
        echo.
        echo 完成! 静态网站已生成到 site/ 文件夹
        echo.
    )
) else if "%choice%"=="3" (
    exit /b 0
) else (
    echo 无效的选择
    pause
    exit /b 1
)

pause
