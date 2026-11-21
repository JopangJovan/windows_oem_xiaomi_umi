@echo off
echo ============================================================
echo  Xiaomi Mi 10 (umi) Windows 11 ARM64 Driver Installer
echo ============================================================
echo.
echo [1/5] Installing test-signing certificate...
certutil -addstore Root "%~dp0Drivers\Cert\umi-test.cer"
certutil -addstore TrustedPublisher "%~dp0Drivers\Cert\umi-test.cer"
echo.
echo [2/5] Installing Touchscreen driver (umi-touch)...
pnputil /add-driver "%~dp0Drivers\Touch\umi-touch.inf" /install
echo.
echo [3/5] Installing Buttons driver (umi-buttons)...
pnputil /add-driver "%~dp0Drivers\Buttons\umi-buttons.inf" /install
echo.
echo [4/5] Installing Stereo Speakers driver (umi-audio)...
pnputil /add-driver "%~dp0Drivers\Audio\umi-audio.inf" /install
echo.
echo [5/5] Installing Battery & Charger driver (umi-battery)...
pnputil /add-driver "%~dp0Drivers\Battery\umi-battery.inf" /install
echo.
echo Done! Please reboot Windows 11.
pause
