@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title GoSRecomp setup
echo GoSRecomp - setup
echo.
echo This copies the game data out of YOUR Castlevania: Grimoire of Souls
echo macOS v1.4.2 disk image (.dmg) and converts it for Windows.
echo.
if not exist "%~dp0GoS.exe" goto notrelease
if not exist "%~dp0_tools\overlay.7z" goto notrelease
set "SRC=%~1"
if not defined SRC (
  echo Drag the .dmg onto Setup.bat, or paste its full path here and press Enter:
  set /p "SRC=> "
)
set "SRC=%SRC:"=%"
if not exist "%SRC%" (
  echo Cannot find "%SRC%".
  goto fail
)

set "WORK=%~dp0_extract"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%"

echo Extracting the disk image, this takes a few minutes...
"%~dp0_tools\7z.exe" x -y -bso0 -bsp1 -o"%WORK%" "%SRC%" "*\Castlevania Grimoire of Souls.app\Contents\Resources\Data\*" "*\Castlevania Grimoire of Souls.app\Contents\Frameworks\GameAssembly.dylib" "*\Castlevania Grimoire of Souls.app\Contents\PlugIns\SPFXUnityPlugin.bundle\Contents\MacOS\SPFXUnityPlugin" -r
if errorlevel 2 goto fail

set "APP="
for /f "delims=" %%d in ('dir /s /b /ad "%WORK%\*.app" 2^>nul') do if exist "%%d\Contents\Resources\Data\globalgamemanagers" set "APP=%%d"
if not defined APP (
  echo The disk image does not contain the game's macOS app.
  goto fail
)

echo Copying game data...
if exist "%~dp0GoS_Data" rmdir /s /q "%~dp0GoS_Data"
move "%APP%\Contents\Resources\Data" "%~dp0GoS_Data" >nul
if not exist "%~dp0GoS_Data\globalgamemanagers" goto fail
copy /y "%APP%\Contents\Frameworks\GameAssembly.dylib" "%~dp0GoS_Data\GameAssembly.dylib" >nul
if not exist "%~dp0GoS_Data\GameAssembly.dylib" goto fail

echo Applying the Windows conversion...
"%~dp0_tools\7z.exe" x -y -bso0 -bsp0 -o"%~dp0GoS_Data" "%~dp0_tools\overlay.7z"
if errorlevel 1 goto fail
rem converted game files: patches applied to the files of your disk image
for /f "usebackq delims=" %%f in ("%~dp0GoS_Data\_patches\list.txt") do (
  "%~dp0_tools\gosr_patch.exe" apply "%~dp0GoS_Data\%%f" "%~dp0GoS_Data\_patches\%%f.gpatch" "%~dp0GoS_Data\%%f.new"
  if errorlevel 1 goto wrongversion
  move /y "%~dp0GoS_Data\%%f.new" "%~dp0GoS_Data\%%f" >nul
)
rmdir /s /q "%~dp0GoS_Data\_patches"
rem the effects library: recompiled on Windows, it runs the macOS plugin's code
set "SPFX=%APP%\Contents\PlugIns\SPFXUnityPlugin.bundle\Contents\MacOS\SPFXUnityPlugin"
if exist "%SPFX%" copy /y "%SPFX%" "%~dp0GoS_Data\Plugins\x86_64\SPFXUnityPlugin.macho" >nul
if not exist "%~dp0GoS_Data\Plugins\x86_64\SPFXUnityPlugin.macho" echo Note: the effects library was not found in the disk image; effects stay off.

rmdir /s /q "%WORK%"
echo.
echo Done. Start the game with Play.bat (or "Play with crash log.bat").
pause
exit /b 0

:wrongversion
echo.
echo The game files in this disk image are not the ones GoSRecomp expects.
echo It needs the macOS app, version 1.4.2, unmodified.
goto fail

:fail
echo.
echo Setup failed. Please send a screenshot of this window.
pause
exit /b 1

:notrelease
echo This folder is not a GoSRecomp release.
echo On the GitHub Releases page, download GoSRecomp-x.y.z.zip, not "Source code".
echo.
pause
exit /b 1
