@echo off
cd /d "%~dp0"
if not exist "GoS_Data\GameAssembly.dylib" (
  echo Run Setup.bat first.
  pause
  exit /b 1
)
start "" GoS.exe -force-d3d11
