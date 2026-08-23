@echo off
setlocal
pushd "%~dp0"

echo Preparing the dedicated University map build...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\vanilla_map_extraction\tools\Invoke-UniversityVisualCheck.ps1" %*
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
  echo.
  echo UNIVERSITY VISUAL CHECK FAILED with exit code %RESULT%.
  echo Read the error above and the preparation log path, if one was printed.
) else (
  echo.
  echo University visual-check process exited normally.
)

echo.
pause
popd
exit /b %RESULT%
