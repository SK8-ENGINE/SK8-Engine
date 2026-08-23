@echo off
setlocal
title SK8 Engine Multiplayer Minimum Interpolation Check

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Run-MultiplayerVisualCheck.ps1" -Clients 5 -MinimumInterpolationCheck
set "exit_code=%ERRORLEVEL%"

echo.
if not "%exit_code%"=="0" (
  echo Minimum-interpolation visual-check setup failed with exit code %exit_code%.
  echo Read the error above and the setup-error.txt file in the printed run folder.
) else (
  echo Five clients were launched with the true 0 ms interpolation preset.
  echo Keep this window or note the printed run folder before closing it.
)
echo.
pause
exit /b %exit_code%
