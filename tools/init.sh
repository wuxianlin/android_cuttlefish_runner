#!/usr/bin/env bash
set -euo pipefail

TOOLS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "${BUILD_DIR}"' EXIT

sudo apt update

ls -l /dev/kvm || true

sudo apt install -y \
  build-essential \
  libasound2t64 \
  libpulse0 \
  libvncserver-dev \
  libwayland-bin \
  libwayland-dev \
  pkg-config

# Ubuntu 26.04 ships this library as libxml2-16. cuttlefish-base 1.57.x still
# depends on the old package name libxml2; its binaries do not link libxml2,
# so a transitional package is enough for apt.
if ! dpkg-query -W -f='${db:Status-Status}' libxml2 2>/dev/null | grep -qx installed \
  && ! apt-cache pkgnames libxml2 | grep -qx libxml2; then
  libxml2_compat="${BUILD_DIR}/libxml2-compat"
  mkdir -p "${libxml2_compat}/DEBIAN"
  cat > "${libxml2_compat}/DEBIAN/control" << 'EOF'
Package: libxml2
Version: 2.9.14+dfsg-0local1
Section: oldlibs
Priority: optional
Architecture: all
Depends: libxml2-16
Maintainer: android-cuttlefish-runner <noreply@localhost>
Description: Transitional package name for libxml2-16
 Ubuntu 26.04 replaced the libxml2 binary package with libxml2-16.
 cuttlefish-base still declares a dependency on the former name.
EOF
  dpkg-deb --build "${libxml2_compat}" "${BUILD_DIR}/libxml2-compat.deb"
  sudo apt install -y "${BUILD_DIR}/libxml2-compat.deb"
fi

sudo apt install -y cuttlefish-base cuttlefish-user cuttlefish-orchestration

wayland-scanner client-header \
  "${TOOLS_DIR}/virtio-gpu-metadata-v1.xml" \
  "${BUILD_DIR}/virtio-gpu-metadata-v1-client-protocol.h"
wayland-scanner private-code \
  "${TOOLS_DIR}/virtio-gpu-metadata-v1.xml" \
  "${BUILD_DIR}/virtio-gpu-metadata-v1.c"
cc -O2 -Wall -Wextra -I"${BUILD_DIR}" \
  "${TOOLS_DIR}/vnc_wayland_bridge.c" \
  "${BUILD_DIR}/virtio-gpu-metadata-v1.c" \
  -o "${TOOLS_DIR}/cf-vnc-wayland-bridge" \
  $(pkg-config --cflags --libs wayland-client libvncclient)
chmod 0755 \
  "${TOOLS_DIR}/cf-vnc-wayland-bridge" \
  "${TOOLS_DIR}/cvd_runtime.py" \
  "${TOOLS_DIR}/qemu/qemu-system-aarch64"

echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' | sudo tee /etc/udev/rules.d/99-kvm4all.rules

sudo udevadm control --reload-rules
sudo udevadm trigger

sudo usermod -aG kvm,cvdnetwork,render $USER