# Userspace NBD Ramdisk

This directory contains a userspace ramdisk daemon with two implemented paths:

- Phase 1: expose the ramdisk memory as a Linux NBD block device.
- Phase 2: expose a control socket for explicit URMA-style peer management and transfer requests.

The URMA path is disabled by default and does not pass through the filesystem. It shares the same backend memory as NBD, but is triggered only by control-plane commands.

## Build

```sh
cd userspace-nbd
make
```

On Linux this builds:

```text
build/nbd-ramdisk
build/ramdiskctl
build/examples/business_client_example
```

On macOS/non-Linux hosts, unit tests and `ramdiskctl` build, but the NBD daemon is intentionally skipped because it requires Linux NBD ioctls.

## Unit Tests

```sh
cd userspace-nbd
make test
```

Covered cases:

- backend create/read/write/zero/bounds checks,
- mock URMA peer connect and both transfer directions,
- alignment and range rejection,
- multiple parallel URMA transfer submissions,
- Unix control socket status/enable/peer-connect path.

## Linux Manual Run

```sh
cd userspace-nbd
sudo modprobe nbd max_part=8
sudo ./build/nbd-ramdisk \
  --device /dev/nbd0 \
  --size 128M \
  --control-sock /tmp/nbd-ramdisk-control.sock \
  --enable-urma
```

In another shell:

```sh
./build/ramdiskctl --sock /tmp/nbd-ramdisk-control.sock status
sudo ln -sf /dev/nbd0 /dev/ramdisk
sudo mkfs.ext4 -F /dev/ramdisk
sudo mkdir -p /mnt/ramdisk
sudo mount /dev/ramdisk /mnt/ramdisk
echo hello | sudo tee /mnt/ramdisk/a.txt
sudo cat /mnt/ramdisk/a.txt
sudo umount /mnt/ramdisk
```

## URMA Control Plane

The control protocol is defined in `include/ramdisk_ctrl.h`.

Basic commands:

```sh
./build/ramdiskctl --sock /tmp/nbd-ramdisk-control.sock enable
./build/ramdiskctl --sock /tmp/nbd-ramdisk-control.sock peer-connect 7 00000000000000000000ffff0a000001 0 0x100000 0x100000 11 22
./build/ramdiskctl --sock /tmp/nbd-ramdisk-control.sock transfer 7 1 0 0x100000 4096 to-hbm
./build/ramdiskctl --sock /tmp/nbd-ramdisk-control.sock transfer 7 2 0 0x100000 4096 from-hbm
```

`ramdiskctl` is only a debugging tool. A real userspace service can link the small client helper in `include/ramdisk_ctrl_client.h` and call the control socket directly. See:

```text
examples/business_client_example.c
```

Run the example with defaults:

```sh
./build/examples/business_client_example /tmp/nbd-ramdisk-control.sock \
  7 00000000000000000000ffff0a000001 0 \
  0x100000 0x100000 11 22 \
  0 0x100000 4096
```

The example performs:

```text
query status
enable URMA path
peer-connect with EID/UASID/segment/jetty
transfer ramdisk -> HBM
transfer HBM -> ramdisk
query status
```

The default build uses a mock URMA provider so queueing, bounds checks, and data-direction semantics can be tested without hardware.

To build the real UMDK provider, point the build at UMDK headers and liburma:

```sh
make HAVE_URMA=1 \
  URMA_ROOT=../external/umdk/src/urma \
  URMA_LIBDIR=/path/to/liburma \
  URMA_LIBS="-lurma"
```

Then start with real URMA enabled:

```sh
sudo ./build/nbd-ramdisk \
  --device /dev/nbd0 \
  --size 128M \
  --enable-urma \
  --real-urma \
  --urma-dev <dev-name> \
  --eid-index 0 \
  --urma-trans-mode rc \
  --urma-tp-type rtp
```

The real path follows the UMDK sample flow: `urma_init`, device/EID selection, context/JFC/JFR/Jetty creation, registration of the ramdisk backend memory, peer segment/jetty import, optional RC bind, READ/WRITE WR post, and JFC polling for completion.

## Linux Integration Test

```sh
cd userspace-nbd
DEVICE=/dev/nbd0 MOUNT_DIR=/mnt/ramdisk sudo -E ./scripts/linux_integration_test.sh
```
