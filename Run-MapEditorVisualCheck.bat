@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Run-MapEditorVisualCheck.ps1" %*
if errorlevel 1 (
  echo.
  echo MAP EDITOR VISUAL CHECK FAILED.
  pause
  exit /b 1
)
exit /b 0
