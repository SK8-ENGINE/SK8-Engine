@echo off
setlocal
title SK8 Engine Multiplayer High Interpolation Check

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Run-MultiplayerVisualCheck.ps1" -Clients 5 -HighInterpolationCheck
set "exit_code=%ERRORLEVEL%"

echo.
if not "%exit_code%"=="0" (
  echo High-interpolation visual-check setup failed with exit code %exit_code%.
  echo Read the error above and the setup-error.txt file in the printed run folder.
) else (
  echo Five clients were launched with the 250 ms high-interpolation preset.
  echo Keep this window or note the printed run folder before closing it.
)
echo.
pause
exit /b %exit_code%
