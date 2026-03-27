@echo off
chcp 65001 >nul
cd /d "e:\blender_bulid_test\blender_npr_bulid\blender-5.1-npr-doc-site"
powershell -ExecutionPolicy Bypass -File .\deploy-to-github.ps1
pause
