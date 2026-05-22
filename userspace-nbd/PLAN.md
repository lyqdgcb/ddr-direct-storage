# Userspace NBD Ramdisk Plan

## Goal

Build a userspace ramdisk daemon that exposes a Linux block device through NBD.

Target user-visible flow:

```bash
sudo modprobe nbd max_part=8
sudo ./nbd-ramdisk --device /dev/nbd0 --size 1G
sudo ln -sf /dev/nbd0 /dev/ramdisk
sudo mkfs.ext4 -F /dev/ramdisk
sudo mkdir -p /mnt/ramdisk
sudo mount /dev/ramdisk /mnt/ramdisk
```

This first phase does not integrate NVMe-oF, URMA, HBM, or jetty operations. It focuses on a stable userspace block device that can be formatted and mounted by a normal Linux filesystem.

## Why NBD

NBD lets a userspace process back a kernel block device. The kernel exposes `/dev/nbdX`; the daemon owns the storage memory and serves read/write requests.

Advantages:

- Simple and mature Linux interface.
- Easy to prototype and debug.
- Works with standard filesystems such as ext4 and xfs.
- Keeps ramdisk memory ownership in this project.

Tradeoffs:

- More context switches than an in-kernel block driver.
- Lower peak performance than ublk or kernel blk-mq.
- Does not naturally carry NVMe-oF custom fields such as EID, jetty, or HBM address.

## Proposed Layout

```text
userspace-nbd/
  Makefile
  README.md
  PLAN.md
  nbd_protocol.h
  nbd_ramdisk.c
  ramdisk_backend.h
  ramdisk_backend.c
  test_backend.c
```

Keep existing kernel implementations:

```text
kernel/        RDMA kernel module prototype
kernel-urma/   URMA kernel module prototype
userspace-nbd/ NBD userspace ramdisk prototype
```

## Core Components

### Ramdisk Backend

Owns memory and enforces bounds.

```c
struct ramdisk_backend {
    void *base;
    uint64_t size;
    uint32_t block_size;
    pthread_mutex_t lock;
};
```

Proposed API:

```c
int ramdisk_backend_create(struct ramdisk_backend *b, uint64_t size);
void ramdisk_backend_destroy(struct ramdisk_backend *b);

int ramdisk_backend_read(struct ramdisk_backend *b,
                         uint64_t offset,
                         void *buf,
                         uint32_t len);

int ramdisk_backend_write(struct ramdisk_backend *b,
                          uint64_t offset,
                          const void *buf,
                          uint32_t len);
```

Required checks:

- Reject NULL pointers.
- Reject zero-size backend.
- Reject `offset + len` overflow.
- Reject ranges beyond backend size.
- Lock read/write operations in the first implementation.

### NBD Daemon

Responsibilities:

1. Parse CLI arguments: `--device`, `--size`, optional `--block-size`.
2. Open `/dev/nbdX`.
3. Create `socketpair(AF_UNIX, SOCK_STREAM, 0, socks)`.
4. Configure NBD with ioctls:
   - `NBD_SET_SIZE`
   - `NBD_SET_BLKSIZE`
   - `NBD_SET_FLAGS`
   - `NBD_SET_SOCK`
5. Start one thread blocked in `NBD_DO_IT`.
6. Main thread serves NBD requests on the userspace socket.
7. On shutdown:
   - `NBD_DISCONNECT`
   - `NBD_CLEAR_SOCK`
   - `NBD_CLEAR_QUE`
   - join worker thread
   - release backend memory

## NBD Request Handling

Supported commands for phase 1:

| Command | Behavior |
|---------|----------|
| `NBD_CMD_READ` | Copy data from backend memory to reply payload |
| `NBD_CMD_WRITE` | Read payload and copy into backend memory |
| `NBD_CMD_FLUSH` | Return success |
| `NBD_CMD_DISC` | Return success and exit serve loop |
| `NBD_CMD_TRIM` | Optional: zero requested range or return success |

Every request must log:

- command type
- handle
- offset
- length
- result or errno

## Device Name

The real kernel device will be `/dev/nbdX`.

For `/dev/ramdisk`, use a symlink in phase 1:

```bash
sudo ln -sf /dev/nbd0 /dev/ramdisk
```

Later this can be automated through a udev rule.

## Testing

### Unit Tests

Backend tests:

```bash
make test
```

Coverage:

- Create/destroy backend.
- Write then read back data.
- Reject out-of-bounds reads.
- Reject out-of-bounds writes.
- Reject `offset + len` overflow.
- Verify unrelated ranges remain unchanged.

### Manual Linux Integration Test

Requires Linux and root:

```bash
sudo modprobe nbd max_part=8
sudo ./nbd-ramdisk --device /dev/nbd0 --size 128M
sudo mkfs.ext4 -F /dev/nbd0
sudo mkdir -p /mnt/ramdisk
sudo mount /dev/nbd0 /mnt/ramdisk
echo hello | sudo tee /mnt/ramdisk/a.txt
cat /mnt/ramdisk/a.txt
sudo umount /mnt/ramdisk
```

### Stress Test

```bash
fio --name=ramdisk \
    --filename=/mnt/ramdisk/fio.bin \
    --rw=randrw \
    --bs=4k \
    --size=64M \
    --iodepth=16 \
    --numjobs=4
```

## Error Handling Requirements

The daemon must fail safely:

- Invalid request magic: log and disconnect.
- Payload read short: log and disconnect.
- Backend range error: return NBD EIO response.
- Allocation failure: exit before attaching the device.
- SIGINT/SIGTERM: disconnect NBD, clear socket/queue, free memory.
- Daemon exit must not leave the NBD socket attached.

## Relationship To URMA

Phase 1 does not register memory with URMA.

Future phase:

```text
ramdisk_backend memory
        |
        v
userspace liburma registration
        |
        v
URMA read/write to HBM
```

The LBA/offset mapping and backend bounds checks should remain reusable when URMA is added.
