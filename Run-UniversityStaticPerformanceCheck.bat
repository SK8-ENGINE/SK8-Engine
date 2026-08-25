@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Run-MapEditorVisualCheck.ps1" -UniversityPerformanceMode Static %*
if errorlevel 1 (
  echo.
  echo UNIVERSITY STATIC PERFORMANCE CHECK FAILED.
  pause
  exit /b 1
)
exit /b 0
