@echo off
setlocal
pushd "%~dp0"

echo Launching University visuals with retail-only collision...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\vanilla_map_extraction\tools\Launch-UniversityVisualCheck.ps1" -RetailCollisionOnly
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
  echo.
  echo UNIVERSITY RETAIL-COLLISION CHECK FAILED with exit code %RESULT%.
  echo Read the error above and the intended runtime log path.
) else (
  echo.
  echo University retail-collision check exited normally.
)

echo.
pause
popd
exit /b %RESULT%
