@echo off
echo Installing Xiaomi Mi 10 Stereo Speaker Driver (umi-audio)...
pnputil /add-driver "%~dp0umi-audio.inf" /install
echo.
echo Done! Check Sound settings for "Xiaomi Mi 10 Stereo Speakers".
echo Log file: C:\Windows\Temp\umi-audio.log
pause
