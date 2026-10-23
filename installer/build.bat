@echo off
rem Build the NCX Translator installer: builds the add-in, then compiles installer\NCX.iss with Inno Setup 6.
rem Output: installer\Output\NCX-Translator-Setup-<version>.exe
rem Needs: Visual Studio Build Tools (the C# compiler), a SOLIDWORKS 2018 install (its interop libraries),
rem        the core built (core\build.ps1), and Inno Setup 6 (winget install JRSoftware.InnoSetup).
setlocal
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File sw\build.ps1 || goto :fail
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo Inno Setup 6 not found: winget install JRSoftware.InnoSetup
  goto :fail
)
"%ISCC%" installer\NCX.iss || goto :fail
echo.
echo Installer written to installer\Output
exit /b 0
:fail
echo Build failed.
exit /b 1
