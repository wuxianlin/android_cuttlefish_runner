#!/usr/bin/env bash
set -euo pipefail

TOOLS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

echo aosp branch: $BRANCH
echo aosp device: $DEVICE
TARGET=${DEVICE}-userdebug

HOST_ARCH=$(uname -m)

if [ "$HOST_ARCH" = "aarch64" ]; then
    HOST_ARCH="arm64"
fi

if [[ "$DEVICE" == *"arm64"* ]]; then
    TARGET_ARCH="arm64"
elif [[ "$DEVICE" == *"x86_64"* ]]; then
    TARGET_ARCH="x86_64"
elif [[ "$DEVICE" == *"riscv64"* ]]; then
    TARGET_ARCH="riscv64"
else
    echo "unknown arch from DEVICE ($DEVICE)"
    exit 1
fi

echo "-> host arch: $HOST_ARCH"
echo "-> target arch: $TARGET_ARCH"

# /builds/latest/.../BUILD_INFO no longer redirects to a build id.
VERSION=$(curl -fsSL "https://ci.android.com/builds/branches/${BRANCH}/status.json" | python3 -c '
import json, sys
target = sys.argv[1]
data = json.load(sys.stdin)
for item in data.get("targets", []):
    if item.get("name") == target:
        build_id = str(item.get("last_known_good_build") or "")
        if build_id and build_id != "0":
            print(build_id)
            sys.exit(0)
        sys.exit(f"no last_known_good_build for {target}")
sys.exit(f"target not in status.json: {target}")
' "$TARGET")
echo "-> build id: ${VERSION}"

ARTIFACT_BASE="https://ci.android.com/builds/submitted/${VERSION}/${TARGET}/latest/raw"
wget -nv "${ARTIFACT_BASE}/${DEVICE}-img-${VERSION}.zip" -O "${DEVICE}-img.zip"

if [ "$TARGET_ARCH" = "$HOST_ARCH" ]; then
    FILE_NAME="cvd-host_package.tar.gz"
    echo "-> arch matched: (run $TARGET_ARCH on $HOST_ARCH)), downloading $FILE_NAME..."
    wget -nv "${ARTIFACT_BASE}/cvd-host_package.tar.gz" -O cvd-host_package.tar.gz
elif [ "$TARGET_ARCH" != "x86_64" ] && [ "$HOST_ARCH" = "x86_64" ]; then
    FILE_NAME="cvd-host_package-x86_64.tar.gz"
    echo "->  cross arch detected:  (run $TARGET_ARCH on $HOST_ARCH), downloading $FILE_NAME..."
    wget -nv "${ARTIFACT_BASE}/cvd-host_package-x86_64.tar.gz" -O cvd-host_package.tar.gz
else
    echo "-> arch not matched: (not support run $TARGET_ARCH on $HOST_ARCH)"
    exit 1
fi

mkdir -p $CF_HOME
tar -xvf cvd-host_package.tar.gz -C "$CF_HOME"
unzip "${DEVICE}-img.zip" -d "$CF_HOME"

if [ "$TARGET_ARCH" != "$HOST_ARCH" ]; then
  install -m 0644 \
    "${TOOLS_DIR}/multi_touchscreen_template.json" \
    "$CF_HOME/etc/default_input_devices/multi_touchscreen_template.json"
fi

if [ ! -e /dev/kvm ] || [ "$TARGET_ARCH" != "$HOST_ARCH" ]; then
  echo "disable kvm"
  sed -i 's/,accel=kvm/\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00/g' $CF_HOME/bin/run_cvd
  sed -i 's/,accel=kvm/\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00/g' $CF_HOME/bin/assemble_cvd
  sed -i "s/,gic-version=3/,gic-version=2/g" $CF_HOME/bin/run_cvd
  sed -i "s/,gic-version=3/,gic-version=2/g" $CF_HOME/bin/assemble_cvd
  sed -i 's/\x00host\x00/\x00max\x00\x00/g' $CF_HOME/bin/run_cvd
  sed -i 's/\x00host\x00/\x00max\x00\x00/g' $CF_HOME/bin/assemble_cvd
  sed -i 's/\x00-accel\x00/\x00-name\x00\x00/g' $CF_HOME/bin/run_cvd
  sed -i 's/\x00-accel\x00/\x00-name\x00\x00/g' $CF_HOME/bin/assemble_cvd
  touch $CF_HOME/use_qemu
fi
