@echo off
echo Installing Xiaomi Mi 10 QCA6390 Bluetooth Driver (umi-bt)...
pnputil /add-driver "%~dp0umi-bt.inf" /install
echo.
echo Checking Bluetooth devices...
powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.InstanceId -match 'UMI0007|UMI_BT|BTH' } | Format-Table Status,Class,FriendlyName,InstanceId -AutoSize"
echo.
if exist C:\Windows\Temp\umi-bt.log (
  echo === C:\Windows\Temp\umi-bt.log ===
  type C:\Windows\Temp\umi-bt.log
)
pause
