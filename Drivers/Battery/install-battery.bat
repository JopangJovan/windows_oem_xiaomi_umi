@echo off
echo Installing Xiaomi Mi 10 Battery Driver (umi-battery)...
pnputil /add-driver "%~dp0umi-battery.inf" /install
echo.
echo Done! Check the taskbar battery icon and Settings -> System -> Power & battery.
echo Log file: C:\Windows\Temp\umi-battery.log
pause
