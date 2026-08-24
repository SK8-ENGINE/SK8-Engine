@echo off
setlocal
title SK8 Engine Mixed-Version Appearance Check

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Run-MultiplayerMixedVersionCheck.ps1" %*
set "exit_code=%ERRORLEVEL%"

echo.
if not "%exit_code%"=="0" (
  echo Mixed-version visual-check setup failed with exit code %exit_code%.
  echo Read the error above and setup-error.txt in the printed run folder.
) else (
  echo The updated client was launched for your manual Steam test.
  echo Keep this window or note the printed run folder before closing it.
)
echo.
pause
exit /b %exit_code%
