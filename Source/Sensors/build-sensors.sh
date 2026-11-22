#!/usr/bin/env bash
set -eu
cd /home/jopvan/Mu-Silicium
wdk=Windows22H2/wdk-arm64/package/c
sdk=Windows22H2/wdk-arm64/sdk-common/c
out=Platforms/Xiaomi/umiPkg/Sensors
pkg="$out/package"
oss=Windows22H2/osslsigncode-build/osslsigncode
gencat=/home/jopvan/Mu-Silicium/Windows22H2/generate-cat-file/gencat.sh
cert=Windows22H2/umi-test-certificate/umi-test.crt
key=Windows22H2/umi-test-certificate/umi-test.key

mkdir -p "$pkg"

CFLAGS=(
  --target=aarch64-pc-windows-msvc
  -fms-extensions -fms-compatibility
  -Wno-invalid-token-paste -Wno-pragma-pack -Wno-nonportable-include-path
  -Wno-deprecated-declarations -Wno-visibility -Wno-microsoft-anon-tag
  -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic
  -Wno-microsoft-enum-forward-reference -Wno-unknown-pragmas
  -ffreestanding -O2
  -DKMDF_VERSION_MAJOR=1 -DKMDF_VERSION_MINOR=31 -DKMDF_MINIMUM_VERSION_REQUIRED=31
  -D_ARM64_ -D_KERNEL_MODE -DNTDDI_VERSION=0x0A00000B -D_WIN32_WINNT=0x0A00
  -I "$out"
  -I "$wdk/Include/10.0.26100.0/km"
  -I "$wdk/Include/10.0.26100.0/km/crt"
  -I "$sdk/Include/10.0.26100.0/shared"
  -I "$wdk/Include/wdf/kmdf/1.31"
)

clang "${CFLAGS[@]}" -c "$out/umi_slpi.c" -o "$out/umi_slpi.obj"
clang "${CFLAGS[@]}" -c "$out/umi_qmi_sns.c" -o "$out/umi_qmi_sns.obj"
clang "${CFLAGS[@]}" -c "$out/umi_fastrpc.c" -o "$out/umi_fastrpc.obj"
clang "${CFLAGS[@]}" -c "$out/umi_hid_sns.c" -o "$out/umi_hid_sns.obj"
clang "${CFLAGS[@]}" -c "$out/umi_sensors_main.c" -o "$out/umi_sensors_main.obj"

lld-link /driver /subsystem:native,10.00 /osversion:10.0 /version:10.0 \
  /dynamicbase /nxcompat /tsaware:no /machine:arm64 /entry:FxDriverEntry /nodefaultlib \
  /out:"$pkg/umi-sensors.sys" \
  "$out/umi_slpi.obj" "$out/umi_qmi_sns.obj" "$out/umi_fastrpc.obj" "$out/umi_hid_sns.obj" "$out/umi_sensors_main.obj" \
  "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfdriverentry.lib" \
  "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfldr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/hal.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/vhfkm.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/libcntpr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/bufferoverflowfastfailk.lib"

cp "$out/umi-sensors.inf" "$pkg/umi-sensors.inf"
cp "$out/install-sensors.bat" "$pkg/install-sensors.bat"

"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-sensors.sys" -out "$pkg/umi-sensors.sys.signed"
mv "$pkg/umi-sensors.sys.signed" "$pkg/umi-sensors.sys"

(cd "$pkg" && "$gencat" -o umi-sensors.cat -h 'ACPI\UMI0009' -O '_v100_ARM64' -A '2:10.0' umi-sensors.inf umi-sensors.sys)
"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-sensors.cat" -out "$pkg/umi-sensors.cat.signed"
mv "$pkg/umi-sensors.cat.signed" "$pkg/umi-sensors.cat"

iasl -p "$out/umi-sensors" "$out/umi-sensors.asl"
cp "$out/umi-sensors.aml" Silicium-ACPI/Platforms/Xiaomi/umi/umi-sensors.aml
ls -lh "$pkg/umi-sensors.sys" "$pkg/umi-sensors.cat" Silicium-ACPI/Platforms/Xiaomi/umi/umi-sensors.aml
