#!/usr/bin/env bash
set -eu
cd /home/jopvan/Mu-Silicium
wdk=Windows22H2/wdk-arm64/package/c
sdk=Windows22H2/wdk-arm64/sdk-common/c
out=Platforms/Xiaomi/umiPkg/Bluetooth
pkg="$out/package"
oss=Windows22H2/osslsigncode-build/osslsigncode
gencat=/home/jopvan/Mu-Silicium/Windows22H2/generate-cat-file/gencat.sh
cert=Windows22H2/umi-test-certificate/umi-test.crt
key=Windows22H2/umi-test-certificate/umi-test.key
btfw=/home/jopvan/.gemini/antigravity-cli/brain/c75f59dd-4bb3-489d-87ac-537346622fd2/scratch/btfw

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
  -c "$out/umi_bt.c" -o "$out/umi_bt.obj"

lld-link /driver /subsystem:native,10.00 /osversion:10.0 /version:10.0 \
  /dynamicbase /nxcompat /tsaware:no /machine:arm64 /entry:FxDriverEntry /nodefaultlib \
  /out:"$pkg/umi-bt.sys" \
  "$out/umi_bt.obj" \
  "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfdriverentry.lib" \
  "$wdk/Lib/wdf/kmdf/ARM64/1.31/wdfldr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/hal.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/libcntpr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/bufferoverflowfastfailk.lib"

cp "$out/umi-bt.inf" "$pkg/umi-bt.inf"
cp "$out/install-bt.bat" "$pkg/install-bt.bat"
cp "$btfw/htbtfw20.tlv" "$pkg/htbtfw20.tlv"
cp "$btfw/htnv20.bin" "$pkg/htnv20.bin"

"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-bt.sys" -out "$pkg/umi-bt.sys.signed"
mv "$pkg/umi-bt.sys.signed" "$pkg/umi-bt.sys"

(cd "$pkg" && "$gencat" -o umi-bt.cat -h 'ACPI\UMI0007' -O '_v100_ARM64' -A '2:10.0' umi-bt.inf umi-bt.sys htbtfw20.tlv htnv20.bin)
"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-bt.cat" -out "$pkg/umi-bt.cat.signed"
mv "$pkg/umi-bt.cat.signed" "$pkg/umi-bt.cat"

iasl -p "$out/umi-bt" "$out/umi-bt.asl"
cp "$out/umi-bt.aml" Silicium-ACPI/Platforms/Xiaomi/umi/umi-bt.aml
ls -lh "$pkg/umi-bt.sys" "$pkg/umi-bt.cat" Silicium-ACPI/Platforms/Xiaomi/umi/umi-bt.aml
