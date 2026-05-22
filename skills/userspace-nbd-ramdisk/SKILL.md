---
name: userspace-nbd-ramdisk
description: Use this skill when working on the userspace NBD ramdisk implementation, including the /dev/nbdX block-device daemon, /dev/ramdisk symlink workflow, URMA control socket, peer EID/jetty/segment metadata handling, mock or real UMDK URMA providers, business-process integration, and tests under userspace-nbd/.
---

# Userspace NBD Ramdisk With URMA Control Path

## Project Summary

This project implements a userspace ramdisk that can be exposed as a Linux block device through NBD:

```text
filesystem -> /dev/ramdisk -> /dev/nbdX -> nbd-ramdisk daemon -> userspace memory
```

It also implements an explicit URMA special path:

```text
business process / control tool
    -> Unix control socket
    -> peer metadata / transfer command
    -> URMA READ/WRITE
    -> same ramdisk backend memory <-> remote HBM
```

The filesystem path and URMA path share the same backend memory, but URMA IO is never inferred from ordinary filesystem IO. URMA is triggered only by control-plane commands.

## Code Layout

Primary directory:

```text
userspace-nbd/
```

Core files:

- `src/nbd_ramdisk.c`: Linux NBD daemon, `/dev/nbdX` attach, request loop, cleanup.
- `src/ramdisk_backend.c`: userspace memory backend, bounds checks, mutex, read/write/zero.
- `src/ramdisk_urma.c`: URMA manager, queueing, peer table, mock provider, real UMDK provider.
- `src/ramdisk_ctrl_server.c`: daemon-side Unix control socket.
- `src/ramdisk_ctrl_client.c`: reusable client helper for other userspace processes.
- `src/ramdiskctl.c`: CLI debug/control tool.
- `examples/business_client_example.c`: example business process calling the control socket directly.
- `include/ramdisk_ctrl.h`: wire protocol shared by daemon, CLI, and business processes.
- `include/ramdisk_urma.h`: URMA manager and peer/queue structures.
- `include/ramdisk_ctrl_client.h`: public helper API for business-process integration.
- `tests/`: unit tests for backend, URMA queue, and control socket.
- `DESIGN.md`: design details and capability boundaries.
- `URMA_USERSPACE_NOTES.md`: notes from UMDK `urma_sample.c`.

External reference:

```text
external/umdk/src/urma/examples/urma_sample.c
```

Use it as the source of truth for UMDK object creation and READ/WRITE WR flow.

## Important Architecture Rules

- The ramdisk backend is the source of truth; do not create a second shadow buffer for normal operation.
- NBD filesystem IO uses `ramdisk_backend_read/write/zero`.
- URMA transfer commands use `local_offset` into the same backend memory.
- `peer-connect` must provide peer EID, UASID, remote segment VA/len/token, and jetty ID.
- `transfer` must provide peer ID, request ID, local offset, remote HBM address, length, and direction.
- First implementation requires URMA transfers to be block-size aligned, usually 4 KiB.
- Keep detailed logging on every error path. This code is meant for bring-up on real systems and must fail safely.
- Do not assume filesystem page cache coherence after out-of-band URMA writes. Tests that mix filesystem and URMA should use explicit sync/drop-cache/remount or direct IO.

## Data Directions

Control-plane direction names are from the ramdisk daemon perspective:

| Direction | Meaning | URMA opcode |
|-----------|---------|-------------|
| `RAMDISK_TO_HBM` / `to-hbm` | local ramdisk memory -> remote HBM | `URMA_OPC_WRITE` |
| `HBM_TO_RAMDISK` / `from-hbm` | remote HBM -> local ramdisk memory | `URMA_OPC_READ` |

Preserve this mapping when adding new APIs or tests.

## Control Plane

The control protocol is defined in `include/ramdisk_ctrl.h`.

Current opcodes:

```c
RAMDISK_CTRL_QUERY_STATUS
RAMDISK_CTRL_URMA_ENABLE
RAMDISK_CTRL_URMA_DISABLE
RAMDISK_CTRL_PEER_CONNECT
RAMDISK_CTRL_PEER_DISCONNECT
RAMDISK_CTRL_URMA_TRANSFER
```

`ramdiskctl` is only a debug tool. Real userspace services should link or copy the client helper from:

```text
include/ramdisk_ctrl_client.h
src/ramdisk_ctrl_client.c
examples/business_client_example.c
```

Example business flow:

```c
ramdisk_ctrl_query_status(sock, &status);
ramdisk_ctrl_enable_urma(sock);
ramdisk_ctrl_peer_connect(sock, &peer);
ramdisk_ctrl_urma_transfer(sock, &xfer);
```

## URMA Provider Modes

The daemon supports two provider modes behind the same manager and queue:

| Mode | Build/Run | Purpose |
|------|-----------|---------|
| Mock | default | Runs without URMA hardware; simulates remote HBM with heap memory |
| Real UMDK | `HAVE_URMA=1` plus `--real-urma` | Uses UMDK `urma_*` APIs |

Real UMDK flow in `src/ramdisk_urma.c`:

1. `urma_init`.
2. `urma_get_device_by_name`.
3. `urma_query_device`.
4. `urma_get_eid_list`.
5. `urma_create_context`.
6. `urma_create_jfce`, `urma_create_jfc`, `urma_create_jfr`, `urma_create_jetty`.
7. `urma_register_seg` on `ramdisk_backend_base()`.
8. On peer connect: fill `urma_seg_t`, then `urma_import_seg`.
9. Import remote jetty with `urma_import_jetty`.
10. If RC mode: `urma_bind_jetty`.
11. For transfer: build local/remote SGE, post `urma_post_jetty_send_wr`.
12. Poll completion with `urma_poll_jfc` and match `user_ctx == request_id`.

If UMDK APIs change, update this list and `URMA_USERSPACE_NOTES.md`.

## Build

Default mock build:

```sh
cd userspace-nbd
make
make test
```

Real UMDK build:

```sh
cd userspace-nbd
make HAVE_URMA=1 \
  URMA_ROOT=../external/umdk/src/urma \
  URMA_LIBDIR=/path/to/liburma \
  URMA_LIBS="-lurma"
```

Linux daemon run:

```sh
sudo modprobe nbd max_part=8
sudo ./build/nbd-ramdisk \
  --device /dev/nbd0 \
  --size 128M \
  --control-sock /tmp/nbd-ramdisk-control.sock \
  --enable-urma
```

Real URMA run adds:

```sh
--real-urma --urma-dev <dev-name> --eid-index <n> --urma-trans-mode rc --urma-tp-type rtp
```

Filesystem workflow:

```sh
sudo ln -sf /dev/nbd0 /dev/ramdisk
sudo mkfs.ext4 -F /dev/ramdisk
sudo mkdir -p /mnt/ramdisk
sudo mount /dev/ramdisk /mnt/ramdisk
```

## Test Expectations

Always run:

```sh
cd userspace-nbd
make test
```

Expected tests:

- `test_backend`: allocation, read/write/zero, bounds checks.
- `test_urma_queue`: mock peer connect, both transfer directions, alignment/range rejection, parallel transfers.
- `test_ctrl_server`: control socket status/enable/peer-connect.

On restricted sandbox environments, Unix socket bind may be skipped by the test. On normal host execution it should pass.

For real URMA code-path compile checking without linking:

```sh
cc -Iinclude -DHAVE_URMA=1 \
  -I../external/umdk/src/urma/lib/urma/core/include \
  -O2 -g -Wall -Wextra -Werror -pthread \
  -c src/ramdisk_urma.c -o /tmp/ramdisk_urma_real.o
```

Linux integration test:

```sh
cd userspace-nbd
DEVICE=/dev/nbd0 MOUNT_DIR=/mnt/ramdisk sudo -E ./scripts/linux_integration_test.sh
```

## Development Guidance

- Keep backend memory access protected by the existing mutex unless implementing a deliberate range-lock design.
- Keep queue-depth enforcement and `request_id` completion matching intact.
- Validate every local range with `ramdisk_backend_validate_range`.
- Validate remote ranges against the connected peer segment before posting URMA WRs.
- Keep mock provider behavior semantically identical to real URMA directions.
- Keep `ramdiskctl` and `business_client_example` using `ramdisk_ctrl_client.c`; avoid duplicating socket code.
- If adding asynchronous completion events later, keep the synchronous helper path for tests and simple bring-up.
- If adding new control opcodes, update `include/ramdisk_ctrl.h`, `src/ramdisk_ctrl_server.c`, `src/ramdisk_ctrl_client.c`, tests, and this skill.
