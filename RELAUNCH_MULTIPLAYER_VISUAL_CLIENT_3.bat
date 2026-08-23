@echo off
setlocal
title SK8 Engine Multiplayer Role 3 Relaunch

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Relaunch-MultiplayerVisualClient.ps1" -Role 3
set "exit_code=%ERRORLEVEL%"

echo.
if not "%exit_code%"=="0" (
  echo Role 3 relaunch failed with exit code %exit_code%.
) else (
  echo Role 3 was relaunched in the latest visual-check run.
)
echo.
pause
exit /b %exit_code%
