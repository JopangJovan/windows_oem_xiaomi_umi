@echo off
echo ============================================================
echo  Xiaomi Mi 10 (umi) Windows 11 ARM64 Driver Installer
echo ============================================================
echo.
echo [1/8] Installing test-signing certificate and disabling hibernation...
certutil -f -addstore Root "%~dp0Drivers\Cert\umi-test.cer"
certutil -f -addstore TrustedPublisher "%~dp0Drivers\Cert\umi-test.cer"
powercfg /h off
echo.
echo [2/8] Installing Touchscreen driver (umi-touch)...
pnputil /add-driver "%~dp0Drivers\Touch\umi-touch.inf" /install
echo.
echo [3/8] Installing Buttons driver (umi-buttons)...
pnputil /add-driver "%~dp0Drivers\Buttons\umi-buttons.inf" /install
echo.
echo [4/8] Installing Stereo Speakers driver (umi-audio)...
pnputil /add-driver "%~dp0Drivers\Audio\umi-audio.inf" /install
echo.
echo [5/8] Installing Battery & Charger driver (umi-battery)...
pnputil /add-driver "%~dp0Drivers\Battery\umi-battery.inf" /install
echo.
echo [6/8] Installing QCA6390 Bluetooth driver (umi-bt)...
pnputil /add-driver "%~dp0Drivers\Bluetooth\umi-bt.inf" /install
echo.
echo [7/8] Installing AMOLED Display, Brightness & Night Light driver (umi-display)...
pnputil /add-driver "%~dp0Drivers\Display\umi-display.inf" /install
echo.
echo [8/8] Installing SLPI Sensor Hub driver (umi-sensors)...
pnputil /add-driver "%~dp0Drivers\Sensors\umi-sensors.inf" /install
echo.
echo Configuring Auto-Rotation and USB Device Mode...
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AutoRotation" /v Enable /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AutoRotation" /v SlateEnable /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AutoRotation" /v SensorPresent /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl" /v ConvertibleSlateMode /t REG_DWORD /d 0 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\USB" /v OsDefaultRoleSwitchMode /t REG_DWORD /d 2 /f
echo.
echo Done! Please reboot Windows 11.
pause
