@echo off
rem Drop your Donkey Kong Country ROM (.sfc / .smc / .zip) onto this file.
if "%~1"=="" (
  echo Drag your Donkey Kong Country ROM file onto this .bat file, or use "DKC32X Builder.exe".
  pause
  exit /b 1
)
"%~dp0app\runtime\python\python.exe" "%~dp0app\tools\convert.py" "%~1"
if errorlevel 1 (echo. & echo Conversion FAILED - see the messages above.) else (echo. & echo Done. The .32x file is next to your ROM.)
pause
