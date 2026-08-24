@echo off
setlocal
pushd "%~dp0"

echo Launching University with ONLY shadow-map sampling disabled...
echo Baked lightmaps, diffuse textures, macro overlays, and decals remain enabled.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\vanilla_map_extraction\tools\Launch-UniversityVisualCheck.ps1" -NoShadows
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
  echo.
  echo UNIVERSITY SHADOW ISOLATION CHECK FAILED with exit code %RESULT%.
  echo Read the error above and the intended runtime log path.
) else (
  echo.
  echo University shadow isolation process exited normally.
)

echo.
pause
popd
exit /b %RESULT%
