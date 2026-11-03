@echo off
setlocal
cd /d "%~dp0"
if not exist "GoS_Data\GameAssembly.dylib" (
  echo Run Setup.bat first.
  pause
  exit /b 1
)
set GOSR_VERBOSE=1
set GOSR_TRACE_NRE=1
set "REPORT=%~dp0crash_report"
if exist "%REPORT%" rmdir /s /q "%REPORT%"
mkdir "%REPORT%"
echo Running the game with logging. Play until it crashes or gets stuck, then close it.
echo This window collects the report when the game exits.
GoS.exe -force-d3d11 -logFile "%REPORT%\Player.log"
set "CRASHES=%TEMP%\Konami Digital Entertainment Co., Ltd_\Castlevania_ Grimoire of Souls\Crashes"
if exist "%CRASHES%" robocopy "%CRASHES%" "%REPORT%\Crashes" /E /NFL /NDL /NJH /NJS /NP >nul
if exist "%~dp0gosr_server.log" copy /y "%~dp0gosr_server.log" "%REPORT%\" >nul
if exist "%~dp0gosr_log.txt" copy /y "%~dp0gosr_log.txt" "%REPORT%\" >nul
if exist "%~dp0gosr_spfx.log" copy /y "%~dp0gosr_spfx.log" "%REPORT%\" >nul
dir /s "%~dp0*.dll" > "%REPORT%\files.txt" 2>&1
rem the save (small files) so the problem can be reproduced
set "SAVE=%USERPROFILE%\AppData\LocalLow\Konami Digital Entertainment Co_, Ltd_\Castlevania_ Grimoire of Souls"
if exist "%SAVE%" robocopy "%SAVE%" "%REPORT%\save" /E /XD cache /NFL /NDL /NJH /NJS /NP >nul
if not exist "%REPORT%\save" mkdir "%REPORT%\save"
reg export "HKCU\Software\Konami Digital Entertainment Co., Ltd.\Castlevania: Grimoire of Souls" "%REPORT%\save\prefs.reg" /y >nul 2>&1
if exist "%~dp0savedata" robocopy "%~dp0savedata" "%REPORT%\save\savedata" /E /NFL /NDL /NJH /NJS /NP >nul
dxdiag /t "%REPORT%\dxdiag.txt"
if exist "%~dp0crash_report.zip" del "%~dp0crash_report.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%REPORT%\*' -DestinationPath '%~dp0crash_report.zip' -Force"
echo.
echo Report saved as crash_report.zip next to this file. Send it over.
explorer /select,"%~dp0crash_report.zip"
pause
