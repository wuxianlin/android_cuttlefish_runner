#!/usr/bin/env bash
set -euo pipefail

CVD_HOME=$1
TOOLS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
INSTANCE=${CUTTLEFISH_INSTANCE:-1}

if [ ! -d "$CVD_HOME" ] || [ ! -x "$CVD_HOME/bin/launch_cvd" ]; then
  echo "launch_cvd not found"
  exit 1
fi
cd "$CVD_HOME"

if [ -e use_qemu ]; then
  export CF_HOME="$CVD_HOME"
  export CUTTLEFISH_INSTANCE="$INSTANCE"
  CF_REAL_QEMU=$(find "$CVD_HOME/bin" -type f \
    -path "*/qemu/qemu-system-aarch64" -print -quit)
  if [ -z "$CF_REAL_QEMU" ]; then
    echo "qemu-system-aarch64 not found"
    exit 1
  fi
  export CF_REAL_QEMU

  HOME=$PWD ./bin/launch_cvd \
    --vm_manager=qemu_cli \
    --qemu_binary_dir="$TOOLS_DIR/qemu" \
    --gpu_mode=guest_swiftshader \
    --start_webrtc=true \
    -cpus=4 \
    --memory_mb=8192 \
    -enable_sandbox=false \
    --report_anonymous_usage_stats=n \
    --daemon

  eval "$("$TOOLS_DIR/cvd_runtime.py" "$CVD_HOME" "$INSTANCE")"
  while true; do
    "$TOOLS_DIR/cf-vnc-wayland-bridge" \
      "$CF_FRAME_SOCKET" \
      "127.0.0.1:$CF_VNC_DISPLAY" \
      "$CF_DISPLAY_WIDTH" \
      "$CF_DISPLAY_HEIGHT" || true
    sleep 1
  done
else
  HOME=$PWD ./bin/launch_cvd \
    -cpus=4 \
    --memory_mb=8192 \
    -enable_sandbox=false \
    --report_anonymous_usage_stats=n \
    --daemon
fi
