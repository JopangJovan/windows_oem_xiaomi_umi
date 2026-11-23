@echo off
title Xiaomi Mi 10 (umi) Screen-Off Standby Setup
echo Configuring safe AMOLED Screen-Off Standby (prevents SM8250 S0ix hard-reboot)...
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Power" /v PlatformAoAcOverride /t REG_DWORD /d 0 /f
powercfg /h off
powercfg /change standby-timeout-ac 0
powercfg /change standby-timeout-dc 0
powercfg /change hibernate-timeout-ac 0
powercfg /change hibernate-timeout-dc 0
powercfg /change monitor-timeout-dc 1
powercfg /change monitor-timeout-ac 5
powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 4
powercfg /setdcvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 4
powercfg /setactive SCHEME_CURRENT
echo.
echo Done! Power button and screen timeout will now turn off the AMOLED display cleanly without hard-rebooting.
pause
