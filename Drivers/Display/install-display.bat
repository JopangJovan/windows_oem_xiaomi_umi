@echo off
echo Installing Xiaomi Mi 10 AMOLED Display Driver (umi-display.sys)...
pnputil /add-driver "%~dp0umi-display.inf" /install
echo.
echo Checking Display adapter status...
powershell -NoProfile -Command "Get-PnpDevice -Class Display | Format-Table Status,FriendlyName,InstanceId -AutoSize"
echo.
echo If Display adapter still shows Microsoft Basic Display Adapter, update it manually in Device Manager -> Browse C:\umi-disp
pause
