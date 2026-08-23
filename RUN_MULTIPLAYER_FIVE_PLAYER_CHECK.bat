@echo off
setlocal
title SK8 Engine Five-Player Multiplayer Check

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Run-MultiplayerVisualCheck.ps1" -Clients 5
set "exit_code=%ERRORLEVEL%"

echo.
if not "%exit_code%"=="0" (
  echo Five-player visual-check setup failed with exit code %exit_code%.
  echo Read the error above and the setup-error.txt file in the printed run folder.
) else (
  echo The five clients were launched for your manual visual check.
  echo Keep this window or note the printed run folder before closing it.
)
echo.
pause
exit /b %exit_code%
