#!/usr/bin/env bash
set -eu
cd /home/jopvan/Mu-Silicium
wdk=Windows22H2/wdk-arm64/package/c
sdk=Windows22H2/wdk-arm64/sdk-common/c
out=Platforms/Xiaomi/umiPkg/Display
pkg="$out/package"
oss=Windows22H2/osslsigncode-build/osslsigncode
gencat=/home/jopvan/Mu-Silicium/Windows22H2/generate-cat-file/gencat.sh
cert=Windows22H2/umi-test-certificate/umi-test.crt
key=Windows22H2/umi-test-certificate/umi-test.key

mkdir -p "$pkg"
ln -sf bdd.hxx "$out/BDD.hxx"
ln -sf bdd_errorlog.hxx "$out/BDD_ErrorLog.hxx"

CFLAGS=(
  --target=aarch64-pc-windows-msvc
  -fms-extensions -fms-compatibility
  -fno-exceptions -fno-rtti -fno-threadsafe-statics
  -Wno-invalid-token-paste -Wno-pragma-pack -Wno-nonportable-include-path
  -Wno-deprecated-declarations -Wno-visibility -Wno-microsoft-anon-tag
  -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic
  -Wno-microsoft-enum-forward-reference -Wno-unknown-pragmas
  -Wno-c++11-narrowing -Wno-writable-strings -Wno-switch
  -ffreestanding -O2
  -D_ARM64_ -D_KERNEL_MODE -DNTDDI_VERSION=0x0A00000B -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
  -I "$out"
  -I "$wdk/Include/10.0.26100.0/km"
  -I "$wdk/Include/10.0.26100.0/km/crt"
  -I "$wdk/Include/10.0.26100.0/shared"
  -I "$sdk/Include/10.0.26100.0/shared"
  -I "$sdk/Include/10.0.26100.0/um"
)

OBJS=()
for f in bdd bdd_ddi bdd_dmm bdd_util bltfuncs blthw memory umi_dsi_bl; do
  clang++ "${CFLAGS[@]}" -c "$out/$f.cxx" -o "$out/$f.obj"
  OBJS+=("$out/$f.obj")
done

lld-link /driver /subsystem:native,10.00 /osversion:10.0 /version:10.0 \
  /dynamicbase /nxcompat /tsaware:no /machine:arm64 /entry:DriverEntry /nodefaultlib \
  /out:"$pkg/umi-display.sys" \
  "${OBJS[@]}" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/displib.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/hal.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/libcntpr.lib" \
  "$wdk/Lib/10.0.26100.0/km/ARM64/bufferoverflowfastfailk.lib"

cp "$out/umi-display.inf" "$pkg/umi-display.inf"
cp "$out/install-display.bat" "$pkg/install-display.bat"

"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-display.sys" -out "$pkg/umi-display.sys.signed"
mv "$pkg/umi-display.sys.signed" "$pkg/umi-display.sys"

(cd "$pkg" && "$gencat" -o umi-display.cat -h 'ACPI\VEN_QCOM&DEV_253A' -O '_v100_ARM64' -A '2:10.0' umi-display.inf umi-display.sys)
"$oss" sign -certs "$cert" -key "$key" -h sha256 \
  -in "$pkg/umi-display.cat" -out "$pkg/umi-display.cat.signed"
mv "$pkg/umi-display.cat.signed" "$pkg/umi-display.cat"

ls -lh "$pkg/umi-display.sys" "$pkg/umi-display.cat"
