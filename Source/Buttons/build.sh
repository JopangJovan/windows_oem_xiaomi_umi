#!/usr/bin/env bash
set -eu
cd /home/jopvan/Mu-Silicium
wdk=Windows22H2/wdk-arm64/package/c
sdk=Windows22H2/wdk-arm64/sdk-common/c
out=Platforms/Xiaomi/umiPkg/Buttons
clang --target=aarch64-pc-windows-msvc -fms-extensions -fms-compatibility -Wno-invalid-token-paste -Wno-pragma-pack -Wno-nonportable-include-path -ffreestanding -DKMDF_VERSION_MAJOR=1 -DKMDF_VERSION_MINOR=31 -DKMDF_MINIMUM_VERSION_REQUIRED=31 -D_ARM64_ -D_KERNEL_MODE -DNTDDI_VERSION=0x0A00000B -D_WIN32_WINNT=0x0A00 -c "$out/umi-buttons.c" -o "$out/umi-buttons.obj" -I "$wdk/Include/10.0.26100.0/km" -I "$wdk/Include/10.0.26100.0/km/crt" -I "$sdk/Include/10.0.26100.0/shared" -I "$wdk/Include/wdf/kmdf/1.31"
lld-link /driver /subsystem:native,10.00 /osversion:10.0 /version:10.0 /dynamicbase /nxcompat /tsaware:no /machine:arm64 /entry:FxDriverEntry /nodefaultlib /out:"$out/umi-buttons.sys" "$out/umi-buttons.obj" "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfdriverentry.lib" "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfldr.lib" "$wdk/Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib" "$wdk/Lib/10.0.26100.0/km/ARM64/bufferoverflowfastfailk.lib" "$wdk/Lib/10.0.26100.0/km/ARM64/libcntpr.lib" "$wdk/Lib/10.0.26100.0/km/ARM64/vhfkm.lib" "$wdk/Lib/10.0.26100.0/km/ARM64/hal.lib"
