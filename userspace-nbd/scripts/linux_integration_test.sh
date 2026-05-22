#!/usr/bin/env bash
set -euo pipefail

DEVICE="${DEVICE:-/dev/nbd0}"
MOUNT_DIR="${MOUNT_DIR:-/mnt/ramdisk}"
SIZE="${SIZE:-128M}"
CTRL_SOCK="${CTRL_SOCK:-/tmp/nbd-ramdisk-control.sock}"

cd "$(dirname "$0")/.."
make

sudo modprobe nbd max_part=8
sudo mkdir -p "$MOUNT_DIR"

sudo ./build/nbd-ramdisk \
  --device "$DEVICE" \
  --size "$SIZE" \
  --control-sock "$CTRL_SOCK" \
  --enable-urma &
DAEMON_PID=$!

cleanup() {
  set +e
  sudo umount "$MOUNT_DIR"
  sudo kill "$DAEMON_PID"
  wait "$DAEMON_PID"
}
trap cleanup EXIT

sleep 1
./build/ramdiskctl --sock "$CTRL_SOCK" status
sudo ln -sf "$DEVICE" /dev/ramdisk
sudo mkfs.ext4 -F /dev/ramdisk
sudo mount /dev/ramdisk "$MOUNT_DIR"
echo "hello from nbd ramdisk" | sudo tee "$MOUNT_DIR/hello.txt" >/dev/null
test "$(sudo cat "$MOUNT_DIR/hello.txt")" = "hello from nbd ramdisk"
sync

echo "integration test passed"
