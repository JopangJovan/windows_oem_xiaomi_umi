#!/usr/bin/env bash
set -eu
cd /home/jopvan/Mu-Silicium
wdk=Windows22H2/wdk-arm64/package/c
sdk=Windows22H2/wdk-arm64/sdk-common/c
out=Platforms/Xiaomi/umiPkg/Audio
pkg="$out/package"
oss=Windows22H2/osslsigncode-build/osslsigncode
gencat=/home/jopvan/Mu-Silicium/Windows22H2/generate-cat-file/gencat.sh
cert=Windows22H2/umi-test-certificate/umi-test.crt
key=Windows22H2/umi-test-certificate/umi-test.key

mkdir -p "$pkg"

COMMON=(
  --target=aarch64-pc-windows-msvc
  -fms-extensions -fms-compatibility
  -Wno-invalid-token-paste -Wno-pragma-pack -Wno-nonportable-include-path
  -Wno-deprecated-declarations -Wno-visibility -Wno-microsoft-anon-tag
  -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic
  -Wno-microsoft-enum-forward-reference -Wno-unknown-pragmas
  -ffreestanding -O2
  -D_ARM64_ -D_KERNEL_MODE -DNTDDI_VERSION=0x0A00000B -D_WIN32_WINNT=0x0A00
  -I "$wdk/Include/10.0.26100.0/km"
  -I "$wdk/Include/10.0.26100.0/km/crt"
  -I "$sdk/Include/10.0.26100.0/shared"
)

for f in umi_adsp umi_cs35l41 umi_sound; do
  clang "${COMMON[@]}" -c "$out/$f.c" -o "$out/$f.obj"
done

clang++ "${COMMON[@]}" -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -Wno-inconsistent-missing-override -Wno-overloaded-virtual \
  -c "$out/umi_wavert.cpp" -o "$out/umi_wavert.obj"

lld-link /driver /subsystem:native,10.00 /osversion:10.0 /version:10.0 \
  /dynamicbase /nxcompat /tsaware:no /machine:arm64 /entry:DriverEntry /nodefaultlib \
  /out:"$pkg/umi-audio.sys" \
  "$out/umi_wavert.obj" "$out/umi_adsp.obj" "$out/umi_cs35l41.obj" "$out/umi_sound.obj" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/portcls.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/stdunk.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/ksguid.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/hal.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/libcntpr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/bufferoverflowfastfailk.lib"

cp "$out/umi-audio.inf" "$pkg/umi-audio.inf"
"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-audio.sys" -out "$pkg/umi-audio.sys.signed"
mv "$pkg/umi-audio.sys.signed" "$pkg/umi-audio.sys"

(cd "$pkg" && "$gencat" -o umi-audio.cat -h 'ACPI\UMI0005' -O '_v100_ARM64' -A '2:10.0' umi-audio.inf umi-audio.sys)
"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-audio.cat" -out "$pkg/umi-audio.cat.signed"
mv "$pkg/umi-audio.cat.signed" "$pkg/umi-audio.cat"

iasl -p "$out/umi-audio" "$out/umi-audio.asl"
cp "$out/umi-audio.aml" Silicium-ACPI/Platforms/Xiaomi/umi/umi-audio.aml
ls -lh "$pkg/umi-audio.sys" "$pkg/umi-audio.cat" Silicium-ACPI/Platforms/Xiaomi/umi/umi-audio.aml
