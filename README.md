# Windows Driver pack for Xiaomi Mi 10 (`umi`)

This repository contains driver binary files and sources for **Xiaomi Mi 10 (`umi`, Qualcomm Snapdragon 865 / SM8250)**.
All driver binary files form a board support package to be used on a Xiaomi Mi 10 (`umi`) device to provide hardware support for the Windows 11 ARM64 operating system.

These driver files are not perfect, typos may exist, feel free to file an issue on GitHub in case you found any.

## Hardware Support

| Component | Driver | Status | Notes |
|---|---|---|---|
| **Touchscreen** | `Drivers/Touch/umi-touch.sys` (v0.1.0.10) | ✅ Working | Bit-banged I2C + VHF HID multitouch (`ACPI\UMI0002`) |
| **Side Buttons** | `Drivers/Buttons/umi-buttons.sys` | ✅ Working | SPMI PMIC Arbiter observer + VHF HID (`ACPI\UMI0004`) |
| **WLAN + Internet** | `Platform/WLAN/` (`UmiWlan.efi` + non-coherent ACPI) | ✅ Working | QCA6390 SMMU Stage-1-disabled CB bypass (`SID 0x1c01`) + `PCI0._CCA=0` / IORT |
| **Stereo Speakers** | `Drivers/Audio/umi-audio.sys` (v0.1.0.2) | ✅ Working | PortCls WaveRT + ADSP PIL/GLINK/APR + Dual CS35L41 HALO (`ACPI\UMI0005`) |
| **Battery & Charging** | `Drivers/Battery/umi-battery.sys` (v1.0.0.3) | ✅ Working | KMDF Battery Miniclass + PM8150B Gen4 FG + SMB5 Sink-only (`ACPI\UMI0006`) |
| **Bluetooth** | `Drivers/Bluetooth/umi-bt.sys` (v1.0.0.7) | ✅ Working | KMDF QCA6390 GENI UART6 + Qualcomm IBS + `MS_BTHX_BTHMINI` (`ACPI\UMI0007`) |
| **Display & Brightness** | `Drivers/Display/umi-display.sys` (v1.0.0.1) | ✅ Working | WDDM KMDOD (UEFI GOP) + SM8250 DSI0 `0x51` AMOLED Brightness (`DXGK_BRIGHTNESS_INTERFACE`) |

## Resources

- **Audio Firmware**: Copy your phone's stock ADSP firmware (`adsp.mdt`, `adsp.b00`..`adsp.b18` from `/dev/block/by-name/modem/image/`) and CS35L41 firmware/calibration (`cs35l41-dsp1-spk-prot-revb2.wmfw`, `cs35l41-dsp1-spk-prot-revb2.bin`, `cs35l41-dsp1-spk-prot-RCV-revb2.bin`, `cs35l41_cal.bin`, `cs35l41_cal_right.bin`) to `C:\Windows\System32\drivers\umi-audio\`.
- **USB Charging / Replug**: Switch Windows USB role to Device/Function mode (`UsbFn`):
  ```cmd
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\USB" /v OsDefaultRoleSwitchMode /t REG_DWORD /d 2 /f
  ```
- **ACPI Tables**: Drop `Platform/ACPI/*` into `Silicium-ACPI/Platforms/Xiaomi/umi/` when building UEFI from `JopangJovan/Mu-Silicium`.

## Copyright, License, Disclaimers and end user license agreement

**Below notice must be present in all redistributed portions of this software**

Please see [LICENSE](LICENSE.md)

## Installing manually

For preserving charset encoding, please checkout with using:

```
git clone -c core.autocrlf=false https://github.com/JopangJovan/windows_oem_xiaomi_umi
```

Then run `install-all.bat` as Administrator on Windows 11 ARM64 (with `bcdedit /set testsigning on`).
