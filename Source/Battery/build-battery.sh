#!/usr/bin/env bash
set -eu
cd /home/jopvan/Mu-Silicium
wdk=Windows22H2/wdk-arm64/package/c
sdk=Windows22H2/wdk-arm64/sdk-common/c
out=Platforms/Xiaomi/umiPkg/Battery
pkg="$out/package"
oss=Windows22H2/osslsigncode-build/osslsigncode
gencat=/home/jopvan/Mu-Silicium/Windows22H2/generate-cat-file/gencat.sh
cert=Windows22H2/umi-test-certificate/umi-test.crt
key=Windows22H2/umi-test-certificate/umi-test.key

mkdir -p "$pkg"

clang --target=aarch64-pc-windows-msvc \
  -fms-extensions -fms-compatibility \
  -Wno-invalid-token-paste -Wno-pragma-pack -Wno-nonportable-include-path \
  -Wno-deprecated-declarations -Wno-visibility -Wno-microsoft-anon-tag \
  -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic \
  -Wno-microsoft-enum-forward-reference -Wno-unknown-pragmas \
  -ffreestanding -O2 \
  -DKMDF_VERSION_MAJOR=1 -DKMDF_VERSION_MINOR=31 -DKMDF_MINIMUM_VERSION_REQUIRED=31 \
  -D_ARM64_ -D_KERNEL_MODE -DNTDDI_VERSION=0x0A00000B -D_WIN32_WINNT=0x0A00 \
  -I "$wdk/Include/10.0.26100.0/km" \
  -I "$wdk/Include/10.0.26100.0/km/crt" \
  -I "$sdk/Include/10.0.26100.0/shared" \
  -I "$wdk/Include/wdf/kmdf/1.31" \
  -c "$out/umi_battery.c" -o "$out/umi_battery.obj"

lld-link /driver /subsystem:native,10.00 /osversion:10.0 /version:10.0 \
  /dynamicbase /nxcompat /tsaware:no /machine:arm64 /entry:FxDriverEntry /nodefaultlib \
  /out:"$pkg/umi-battery.sys" \
  "$out/umi_battery.obj" \
  "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfdriverentry.lib" \
  "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfldr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/battc.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/hal.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/libcntpr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/bufferoverflowfastfailk.lib"

cp "$out/umi-battery.inf" "$pkg/umi-battery.inf"
cp "$out/install-battery.bat" "$pkg/install-battery.bat"

"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-battery.sys" -out "$pkg/umi-battery.sys.signed"
mv "$pkg/umi-battery.sys.signed" "$pkg/umi-battery.sys"

(cd "$pkg" && "$gencat" -o umi-battery.cat -h 'ACPI\UMI0006' -O '_v100_ARM64' -A '2:10.0' umi-battery.inf umi-battery.sys)
"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-battery.cat" -out "$pkg/umi-battery.cat.signed"
mv "$pkg/umi-battery.cat.signed" "$pkg/umi-battery.cat"

iasl -p "$out/umi-battery" "$out/umi-battery.asl"
cp "$out/umi-battery.aml" Silicium-ACPI/Platforms/Xiaomi/umi/umi-battery.aml
ls -lh "$pkg/umi-battery.sys" "$pkg/umi-battery.cat" Silicium-ACPI/Platforms/Xiaomi/umi/umi-battery.aml
