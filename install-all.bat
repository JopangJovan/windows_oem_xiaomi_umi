@echo off
echo ============================================================
echo  Xiaomi Mi 10 (umi) Windows 11 ARM64 Driver Installer
echo ============================================================
echo.
echo [1/6] Installing test-signing certificate...
certutil -addstore Root "%~dp0Drivers\Cert\umi-test.cer"
certutil -addstore TrustedPublisher "%~dp0Drivers\Cert\umi-test.cer"
echo.
echo [2/6] Installing Touchscreen driver (umi-touch)...
pnputil /add-driver "%~dp0Drivers\Touch\umi-touch.inf" /install
echo.
echo [3/6] Installing Buttons driver (umi-buttons)...
pnputil /add-driver "%~dp0Drivers\Buttons\umi-buttons.inf" /install
echo.
echo [4/6] Installing Stereo Speakers driver (umi-audio)...
pnputil /add-driver "%~dp0Drivers\Audio\umi-audio.inf" /install
echo.
echo [5/6] Installing Battery & Charger driver (umi-battery)...
pnputil /add-driver "%~dp0Drivers\Battery\umi-battery.inf" /install
echo.
echo [6/7] Installing QCA6390 Bluetooth driver (umi-bt)...
pnputil /add-driver "%~dp0Drivers\Bluetooth\umi-bt.inf" /install
echo.
echo [7/7] Installing AMOLED Display & Brightness driver (umi-display)...
pnputil /add-driver "%~dp0Drivers\Display\umi-display.inf" /install
echo.
echo Done! Please reboot Windows 11.
pause
