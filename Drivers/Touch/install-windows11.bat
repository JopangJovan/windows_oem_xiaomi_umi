@echo off
setlocal
set "LOG=%~dp0install-windows11.log"
echo UMI touch Windows 11 install > "%LOG%"

if "%SystemDrive%"=="X:" (
  echo This is WinPE. Boot the installed Windows 11 first.>>"%LOG%"
  echo Run this from installed Windows 11, as Administrator.
  pause
  exit /b 1
)

fltmc >nul 2>&1
if errorlevel 1 (
  echo Open Command Prompt as Administrator and run this again.>>"%LOG%"
  echo Administrator privileges are required.
  pause
  exit /b 1
)

set "PKG=%~dp0"
set "CERT=%~dp0..\umi-test.cer"
if not exist "%CERT%" set "CERT=%~dp0umi-test.cer"
if not exist "%PKG%umi-touch.inf" goto missing
if not exist "%PKG%umi-touch.sys" goto missing
if not exist "%PKG%umi-touch.cat" goto missing
if not exist "%CERT%" goto missing

echo Checking for ACPI touchscreen node...>>"%LOG%"
pnputil /enum-devices /connected >>"%LOG%" 2>&1
findstr /i /c:"ACPI\UMI0003" "%LOG%" >nul
if errorlevel 1 echo NOTE: ACPI\UMI0003 was not listed before install. Driver will still be staged.>>"%LOG%"

certutil -f -addstore Root "%CERT%" >>"%LOG%" 2>&1
if errorlevel 1 goto failed
certutil -f -addstore TrustedPublisher "%CERT%" >>"%LOG%" 2>&1
if errorlevel 1 goto failed
pnputil /add-driver "%PKG%umi-touch.inf" /install >>"%LOG%" 2>&1
if errorlevel 1 goto failed
pnputil /scan-devices >>"%LOG%" 2>&1
pnputil /enum-devices /connected >>"%LOG%" 2>&1
copy /y "%SystemRoot%\INF\setupapi.dev.log" "%~dp0install-touch-setupapi.log" >>"%LOG%" 2>&1
echo Install command completed. Restart Windows, then test touchscreen.>>"%LOG%"
echo Touchscreen driver staged. Restart Windows and test the screen.
goto finish

:missing
echo Required driver or certificate file is missing.>>"%LOG%"
echo Required driver or certificate file is missing.
goto finish

:failed
echo Installation failed with errorlevel %errorlevel%.>>"%LOG%"
echo Installation failed. See install-windows11.log.

:finish
pause
