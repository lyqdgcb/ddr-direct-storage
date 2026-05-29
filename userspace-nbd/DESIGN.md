# Userspace NBD Ramdisk With URMA Extension Design

## 1. Objective

This document defines the userspace implementation path for the ramdisk storage backend.

The project will be implemented in two phases:

| Phase | Goal | Result |
|-------|------|--------|
| Phase 1 | Userspace NBD ramdisk | `/dev/nbdX` can be symlinked to `/dev/ramdisk`, formatted with a filesystem, and mounted |
| Phase 2 | Userspace URMA data path | The same ramdisk memory can be registered with URMA and used for HBM READ/WRITE through jetty operations |

NVMe-oF target integration is intentionally out of scope for this userspace design. The first deliverable is a stable Linux block device backed by userspace memory.

## 2. References

- `userspace-nbd/PLAN.md`: original Phase 1 NBD plan.
- `userspace-nbd/URMA_USERSPACE_NOTES.md`: analysis of `external/umdk/src/urma/examples/urma_sample.c`.
- `external/umdk`: downloaded UMDK source tree from `https://gitcode.com/openeuler/umdk.git`, analyzed at commit `fb8ec246`.

## 3. Architecture

```text
                  Phase 1

  filesystem
      |
      v
  /dev/ramdisk -> /dev/nbd0
      |
      v
  Linux nbd driver
      |
      v
  userspace nbd-ramdisk daemon
      |
      v
  ramdisk_backend memory
```

```text
                  Phase 2

  userspace nbd-ramdisk daemon
      |
      +--> NBD request path for filesystem IO
      |
      +--> URMA context / JFC / JFR / Jetty
      |
      +--> registered ramdisk segment
      |
      +--> imported remote HBM segment and jetty
      |
      v
  URMA READ/WRITE/SEND/RECV data path
```

The ramdisk backend remains the source of truth in both phases. URMA should register the same memory region owned by `ramdisk_backend`; do not create a second shadow buffer for URMA.

## 3.1 Data Path Separation

There are two independent data paths that share the same backend memory:

```text
Filesystem path:

application read/write
    -> filesystem
    -> /dev/ramdisk
    -> /dev/nbd0
    -> Linux nbd driver
    -> NBD request
    -> ramdisk_backend memcpy
```

```text
URMA special path:

control command
    -> userspace daemon control plane
    -> peer lookup / segment lookup
    -> URMA READ/WRITE
    -> ramdisk_backend memory <-> remote HBM
```

URMA IO does **not** pass through the filesystem and is not inferred from normal NBD read/write requests. A request uses the URMA path only when it arrives through the daemon's control plane as an explicit URMA transfer command.

This separation is intentional:

- Filesystem IO remains ordinary block IO.
- URMA metadata such as EID, jetty, remote segment, remote token, and HBM address is not part of the NBD protocol.
- The daemon can enable, disable, authenticate, validate, queue, and complete URMA transfers independently from filesystem IO.

Both paths can modify the same byte ranges in `ramdisk_backend`, so consistency rules are required. Phase 1 has only filesystem IO. Phase 2 must coordinate filesystem IO and URMA IO.

## 3.2 Capability Boundary

This design intentionally starts conservative. It favors correctness and debuggability before optimizing parallel throughput.

### Phase 1 Capability

Phase 1 provides:

- A userspace ramdisk exposed as `/dev/nbdX`.
- Optional `/dev/ramdisk` symlink.
- Filesystem support through normal Linux block IO.
- Backend read/write bounds checking.
- Safe daemon cleanup on disconnect or signal.

Phase 1 does not provide:

- URMA registration.
- HBM transfer.
- Peer metadata handling.
- Persistent storage.
- Crash recovery after daemon exit.

### Phase 2 Capability

Phase 2 adds:

- Optional userspace URMA initialization.
- Registration of the whole ramdisk backend memory as a local URMA segment.
- Peer metadata import for remote segment and jetty.
- Explicit control-plane URMA transfers.
- Completion matching through `request_id` / `user_ctx`.

Phase 2 does not make URMA IO filesystem-aware. URMA remains a block-level out-of-band path.

### Parallel IO Support

The design can support multiple outstanding IOs, but the first implementation should expose limited parallelism:

| Path | First implementation | Later optimization |
|------|----------------------|--------------------|
| NBD filesystem IO | Single request serving loop or small worker pool | Multi-queue worker pool |
| Backend memory access | Global `pthread_mutex_t` | Range locks |
| URMA transfer submission | Pending queue + bounded inflight map | Per-peer/per-jetty queues |
| URMA completion | Polling or event thread | Batched polling/event-driven completions |

First implementation recommendation:

```text
NBD requests: process serially or with a small worker pool.
URMA requests: allow multiple outstanding WRs up to urma_queue_depth.
Backend memory: serialize overlapping paths with one backend lock.
```

This means the daemon can queue multiple URMA transfers and match completions independently, but actual memory access may still be serialized by the backend lock. This is acceptable for bring-up.

Production direction:

```text
NBD worker pool
    + range lock per backend byte range
    + URMA per-peer queue
    + inflight map: request_id -> transfer
    + completion thread
```

### IO Granularity

The backend is byte-addressable, but exposed block semantics impose practical granularity.

| Layer | Minimum granularity | Recommended granularity | Maximum granularity |
|-------|---------------------|-------------------------|---------------------|
| Backend API | 1 byte | 4 KiB aligned | backend size |
| NBD request | Kernel may send arbitrary byte lengths | 4 KiB / filesystem block size | limited by NBD/kernel request size |
| Filesystem IO | filesystem block/page size | 4 KiB | filesystem/kernel controlled |
| URMA transfer | UMDK SGE length rules | 4 KiB or larger | limited by peer segment, JFC/JFS depth, and max SGE/WR size |

The daemon should accept unaligned NBD offsets and lengths because the kernel may issue them. It should validate and service them correctly.

For URMA transfers, the control plane should initially require:

```text
local_offset % block_size == 0
remote_hbm_addr aligned to 4 KiB or device-required alignment
length % block_size == 0
```

This keeps Phase 2 easier to validate. Later, if UMDK and HBM allow it, the daemon can relax this to byte-granular transfers.

### Size Limits

Phase 1 ramdisk size is limited by userspace virtual memory and available RAM:

```text
size <= available memory / configured process limits
```

Phase 2 ramdisk size is additionally limited by URMA registration capability:

```text
size <= max registerable segment length
size must satisfy device page/alignment requirements
```

The daemon should reject startup if:

- requested size is zero,
- requested size overflows `uint64_t`,
- allocation fails,
- URMA is enabled and segment registration fails.

### Ordering And Consistency

The first implementation should guarantee:

- Each individual backend read/write is bounds-checked and atomic with respect to the backend mutex.
- URMA transfer request completion means the corresponding URMA WR completed successfully or failed with a reported error.
- NBD filesystem IO and URMA IO will not concurrently mutate backend memory while the global backend lock is held.

The first implementation does not guarantee:

- Filesystem page cache coherence after out-of-band URMA writes.
- Ordering between unrelated NBD and URMA requests unless they contend on the backend lock.
- Persistent data after daemon exit.

## 4. Phase 1: Userspace NBD Ramdisk

### 4.1 Deliverables

Add a `userspace-nbd` implementation with:

```text
userspace-nbd/
  Makefile
  README.md
  DESIGN.md
  PLAN.md
  URMA_USERSPACE_NOTES.md
  nbd_protocol.h
  nbd_ramdisk.c
  ramdisk_backend.h
  ramdisk_backend.c
  test_backend.c
```

### 4.2 User Workflow

```bash
sudo modprobe nbd max_part=8
sudo ./nbd-ramdisk --device /dev/nbd0 --size 1G --block-size 4096
sudo ln -sf /dev/nbd0 /dev/ramdisk
sudo mkfs.ext4 -F /dev/ramdisk
sudo mkdir -p /mnt/ramdisk
sudo mount /dev/ramdisk /mnt/ramdisk
```

### 4.3 Ramdisk Backend

The backend owns memory and enforces all bounds.

```c
struct ramdisk_backend {
    void *base;
    uint64_t size;
    uint32_t block_size;
    pthread_mutex_t lock;
};
```

Required API:

```c
int ramdisk_backend_create(struct ramdisk_backend *b,
                           uint64_t size,
                           uint32_t block_size);

void ramdisk_backend_destroy(struct ramdisk_backend *b);

int ramdisk_backend_read(struct ramdisk_backend *b,
                         uint64_t offset,
                         void *buf,
                         uint32_t len);

int ramdisk_backend_write(struct ramdisk_backend *b,
                          uint64_t offset,
                          const void *buf,
                          uint32_t len);

void *ramdisk_backend_base(struct ramdisk_backend *b);
uint64_t ramdisk_backend_size(const struct ramdisk_backend *b);
```

Rules:

- Reject NULL pointers.
- Reject zero-sized backends.
- Reject unsupported block size.
- Reject `offset + len` overflow.
- Reject ranges outside backend memory.
- First implementation may use a single `pthread_mutex_t`.
- Later optimization may use per-range locks or lock-free read path.

### 4.4 NBD Daemon

The daemon should:

1. Parse CLI arguments.
2. Allocate and zero the backend memory.
3. Open `/dev/nbdX`.
4. Create a `socketpair(AF_UNIX, SOCK_STREAM, 0, socks)`.
5. Configure the NBD kernel device:
   - `NBD_SET_SIZE`
   - `NBD_SET_BLKSIZE`
   - `NBD_SET_FLAGS`
   - `NBD_SET_SOCK`
6. Start an NBD worker thread blocked in `NBD_DO_IT`.
7. Serve NBD requests from the userspace socket.
8. On exit, disconnect and clear the NBD device.

Supported commands:

| NBD command | Behavior |
|-------------|----------|
| `NBD_CMD_READ` | Copy backend memory into reply payload |
| `NBD_CMD_WRITE` | Read payload and copy into backend memory |
| `NBD_CMD_FLUSH` | Return success |
| `NBD_CMD_DISC` | Exit serve loop and disconnect |
| `NBD_CMD_TRIM` | Zero range or return success in Phase 1 |

### 4.5 Phase 1 Error Handling

The daemon must fail safely:

- Invalid request magic: log and disconnect.
- Unknown command: reply with EIO.
- Short request header read: log and disconnect.
- Short write payload read: log and disconnect.
- Backend bounds error: reply with EIO.
- Signal received: call `NBD_DISCONNECT`, `NBD_CLEAR_SOCK`, `NBD_CLEAR_QUE`.
- Daemon exit must not leave a socket attached to `/dev/nbdX`.

Every request log should include:

```text
cmd, handle, offset, length, result
```

## 5. Phase 2: Userspace URMA Extension

Phase 2 extends the daemon with optional URMA support. NBD remains the block-device front end; URMA registers and accesses the same backend memory.

URMA is a special control/data path and must be explicitly enabled. It is not triggered by normal filesystem reads or writes.

### 5.0 URMA Enablement And Control Plane

URMA support should be disabled by default. Enable it at daemon startup:

```bash
sudo ./nbd-ramdisk --device /dev/nbd0 --size 1G --enable-urma --urma-dev <dev>
```

If URMA is disabled:

- NBD filesystem IO continues to work.
- Control-plane URMA commands return `EOPNOTSUPP`.
- No URMA memory registration or jetty resources are created.

The daemon should expose a control socket for Phase 2:

```text
/run/nbd-ramdisk/control.sock
```

All peer metadata and URMA transfer requests enter through this control socket. Do not overload NBD read/write requests to carry URMA metadata.

Suggested control opcodes:

```c
enum ramdisk_ctrl_opcode {
    RAMDISK_CTRL_QUERY_STATUS = 1,
    RAMDISK_CTRL_URMA_ENABLE,
    RAMDISK_CTRL_URMA_DISABLE,
    RAMDISK_CTRL_PEER_CONNECT,
    RAMDISK_CTRL_PEER_DISCONNECT,
    RAMDISK_CTRL_URMA_TRANSFER,
};
```

Peer connect command:

```c
struct ramdisk_ctrl_peer_connect_info {
    uint64_t peer_id;
    uint8_t eid[16];
    uint32_t uasid;
    uint64_t seg_va;
    uint64_t seg_len;
    uint32_t seg_token_id;
    uint32_t jetty_id;
};
```

Transfer command:

```c
enum ramdisk_urma_direction {
    RAMDISK_TO_HBM = 1,
    HBM_TO_RAMDISK = 2,
};

struct ramdisk_ctrl_urma_transfer {
    uint64_t peer_id;
    uint64_t request_id;
    uint64_t local_offset;
    uint64_t remote_hbm_addr;
    uint32_t length;
    uint32_t direction;
};
```

The daemon should return a synchronous control-plane status for validation and queueing:

```text
0          request accepted
EINVAL     malformed command or invalid direction
ENODEV     URMA not initialized or peer not connected
EOPNOTSUPP URMA disabled
ERANGE     local or remote range out of bounds
EBUSY      URMA queue is full
```

The actual transfer completion can be reported either by:

- blocking the control request until completion in the first implementation, or
- returning `request_id` immediately and sending a completion event later.

For a simple first implementation, blocking until completion is acceptable if queue depth is small and request timeouts are enforced. For production, use asynchronous completion events.

### 5.1 URMA Objects

Based on `urma_sample.c`, the userspace URMA data path uses:

| Object | Purpose |
|--------|---------|
| `urma_context_t` | Device/EID context |
| `urma_jfce_t` | Completion event channel |
| `urma_jfc_t` | Completion queue |
| `urma_jfr_t` | Receive queue |
| `urma_jetty_t` | Combined send/receive object |
| `urma_target_seg_t` | Local registered or remote imported memory segment |
| `urma_target_jetty_t` | Imported remote jetty target |

### 5.2 URMA Initialization Flow

Process-level initialization:

```c
urma_init_attr_t init_attr = {
    .uasid = 0,
};
urma_init(&init_attr);
```

Per daemon context:

1. `urma_get_device_by_name(dev_name)`.
2. `urma_query_device(dev, &dev_attr)`.
3. `urma_get_eid_list(dev, &eid_cnt)`.
4. Select `eid_index`, then `urma_free_eid_list()`.
5. `urma_create_context(dev, eid_index)`.
6. `urma_create_jfce(ctx)`.
7. `urma_create_jfc(ctx, &jfc_cfg)`.
8. Optionally `urma_rearm_jfc(jfc, false)` for event mode.
9. `urma_create_jfr(ctx, &jfr_cfg)`.
10. `urma_create_jetty(ctx, &jetty_cfg)`.

The daemon should expose CLI options:

```text
--urma
--urma-dev <name>
--eid-index <n>
--urma-trans-mode <rm|rc|um|rs>
--urma-tp-type <rtp|ctp|utp>
--jfc-depth <n>
--jfr-depth <n>
```

### 5.3 Register Ramdisk Memory

Phase 2 must register `ramdisk_backend.base` directly:

```c
urma_reg_seg_flag_t flag = {0};
flag.bs.token_policy = URMA_TOKEN_NONE;
flag.bs.cacheable = URMA_NON_CACHEABLE;
flag.bs.access = URMA_ACCESS_READ |
                 URMA_ACCESS_WRITE |
                 URMA_ACCESS_ATOMIC;

urma_seg_cfg_t seg_cfg = {0};
seg_cfg.va = (uint64_t)backend->base;
seg_cfg.len = backend->size;
seg_cfg.token_value = local_token;
seg_cfg.flag = flag;
seg_cfg.iova = 0;

local_tseg = urma_register_seg(ctx, &seg_cfg);
```

After registration, the daemon can publish:

```text
eid
uasid
seg.ubva.va
seg.len
seg.token_id
jetty_id
```

### 5.4 Control Plane

Use a control channel in Phase 2. `urma_sample.c` uses TCP to exchange peer URMA metadata; this project should use a local Unix domain socket for daemon control and may use TCP only for remote peer metadata exchange.

The control plane exchanges:

```c
struct urma_peer_info {
    /* exact field types should follow UMDK headers */
    eid;
    uasid;
    seg_va;
    seg_len;
    seg_flag;
    seg_token_id;
    jetty_id;
};
```

After receiving peer info:

1. Fill remote segment metadata.
2. Import remote segment with `urma_import_seg()`.
3. Build remote jetty descriptor.
4. Import remote jetty with `urma_import_jetty()`.
5. In RC/RS mode, bind local jetty to remote target jetty with `urma_bind_jetty()`.

The daemon stores peer state:

```c
struct ramdisk_urma_peer {
    uint64_t peer_id;
    /* remote eid and UASID */
    /* imported remote segment */
    /* imported remote target jetty */
    bool bound;
};
```

All `RAMDISK_CTRL_URMA_TRANSFER` commands must reference an existing `peer_id`.

### 5.5 URMA READ/WRITE Data Path

Use READ/WRITE for large data movement.

This path bypasses the filesystem. It directly reads from or writes to `ramdisk_backend` memory using URMA. The filesystem does not see URMA commands and cannot attach metadata such as EID or jetty to them.

Local ramdisk address:

```text
local_va = backend.base + lba * block_size
```

Local UBVA:

```text
local_ubva = local_tseg->seg.ubva.va + lba * block_size
```

Remote HBM address:

```text
remote_hbm_va = remote_seg.ubva.va + hbm_offset
```

Work request pattern:

```c
urma_sge_t local_sge = {
    .addr = local_va_or_ubva,
    .len = len,
    .tseg = local_tseg,
};

urma_sge_t remote_sge = {
    .addr = remote_hbm_va,
    .len = len,
    .tseg = imported_remote_tseg,
};

urma_rw_wr_t rw = {
    .src = src_sg,
    .dst = dst_sg,
};

urma_jfs_wr_t wr = {
    .opcode = URMA_OPC_WRITE or URMA_OPC_READ,
    .flag.bs.complete_enable = 1,
    .tjetty = imported_remote_jetty,
    .user_ctx = request_id,
    .rw = rw,
};

urma_post_jetty_send_wr(local_jetty, &wr, &bad_wr);
```

Direction rules:

| Operation | URMA opcode | Source | Destination |
|-----------|-------------|--------|-------------|
| Push ramdisk data to HBM | `URMA_OPC_WRITE` | local ramdisk SGE | remote HBM SGE |
| Pull HBM data into ramdisk | `URMA_OPC_READ` | remote HBM SGE | local ramdisk SGE |

Local and remote bounds must both be validated before posting:

```text
local_offset + length <= backend.size
remote_hbm_addr >= remote_seg.ubva.va
remote_hbm_addr + length <= remote_seg.ubva.va + remote_seg.len
```

If a transfer overlaps active filesystem IO, Phase 2 first implementation must serialize the operation with the same backend lock used by NBD read/write. Later versions may replace this with range locks.

### 5.5.1 Filesystem Consistency

URMA modifies backend block memory outside the filesystem path. This is a block-level operation, similar to another block writer changing disk contents below a mounted filesystem.

Important consequences:

- Filesystem page cache may contain stale data.
- URMA writes to blocks belonging to a mounted filesystem can bypass filesystem metadata and journaling expectations.
- File-level visibility is not guaranteed unless the test controls cache and synchronization.

Safe Phase 2 testing rules:

1. For block-level URMA validation, read/write `/dev/ramdisk` or `/dev/nbd0` directly with `O_DIRECT` when possible.
2. If testing through mounted files, run `sync` and drop caches or unmount/remount before checking data.
3. Do not use URMA to modify filesystem metadata blocks during early testing.
4. Prefer a reserved raw region outside the formatted filesystem, or use a test file whose block mapping is known and stable.

The design goal is:

```text
NBD path proves the backend can serve filesystems.
URMA path proves the same backend memory can be transferred to/from HBM.
Filesystem coherence with out-of-band URMA writes is a separate policy problem.
```

### 5.6 Completion Handling

Every data WR must set:

```c
wr.flag.bs.complete_enable = 1;
wr.user_ctx = request_id;
```

Completion loop:

1. Poll mode: `urma_poll_jfc(jfc, nr, crs)`.
2. Event mode:
   - `urma_wait_jfc(jfce, ...)`
   - `urma_poll_jfc(jfc, ...)`
   - `urma_ack_jfc(...)`
   - `urma_rearm_jfc(jfc, false)`
3. Check `cr.status == URMA_CR_SUCCESS`.
4. Match request through `cr.user_ctx`.
5. Release or retry request according to status.

Queueing must track outstanding WRs:

```text
pending queue
inflight map: request_id -> request
completion loop: cr.user_ctx -> request
```

### 5.7 SEND/RECV

SEND/RECV is optional for Phase 2 data path, but useful for control messages.

Rules:

- Receiver must post receive buffers before peer sends.
- `urma_post_jetty_recv_wr()` must recycle buffers after receive completion.
- `cr.flag.bs.s_r` distinguishes send-side and receive-side completions.
- `cr.remote_id` can identify the sending peer.

### 5.8 Phase 2 Resource Cleanup

Cleanup order:

1. Stop accepting new NBD or URMA requests.
2. Drain/fail pending URMA requests.
3. Stop completion thread.
4. `urma_unimport_jetty(remote_tjetty)`.
5. `urma_unimport_seg(remote_tseg)`.
6. `urma_unregister_seg(local_tseg)`.
7. `urma_delete_jetty(jetty)`.
8. `urma_delete_jfr(jfr)`.
9. `urma_delete_jfc(jfc)`.
10. `urma_delete_jfce(jfce)`.
11. `urma_delete_context(ctx)`.
12. `urma_uninit()`.
13. Destroy ramdisk backend.

## 6. Verification Plan

### 6.1 Phase 1 Unit Tests

Command:

```bash
make -C userspace-nbd test
```

Cases:

| Case | Expected result |
|------|-----------------|
| Create 128 MiB backend | Success |
| Write then read 4 KiB | Data matches |
| Write unaligned range | Data matches |
| Read beyond end | Error |
| Write beyond end | Error |
| `offset + len` overflow | Error |
| Zero length request | Success or explicit no-op |
| Destroy backend twice | No crash if API permits repeated cleanup |

### 6.2 Phase 1 NBD Filesystem Test

Requires Linux root:

```bash
sudo modprobe nbd max_part=8
sudo ./userspace-nbd/nbd-ramdisk --device /dev/nbd0 --size 128M
sudo ln -sf /dev/nbd0 /dev/ramdisk
sudo mkfs.ext4 -F /dev/ramdisk
sudo mkdir -p /mnt/ramdisk
sudo mount /dev/ramdisk /mnt/ramdisk
echo hello | sudo tee /mnt/ramdisk/a.txt
cat /mnt/ramdisk/a.txt
sudo umount /mnt/ramdisk
```

Expected:

- `mkfs.ext4` succeeds.
- `mount` succeeds.
- File write/read succeeds.
- Daemon logs clean READ/WRITE/FLUSH requests.
- Daemon exits cleanly on disconnect or SIGTERM.

### 6.3 Phase 1 Stress Test

```bash
fio --name=nbd_ramdisk \
    --filename=/mnt/ramdisk/fio.bin \
    --rw=randrw \
    --bs=4k \
    --size=64M \
    --iodepth=16 \
    --numjobs=4 \
    --runtime=60 \
    --time_based
```

Expected:

- No daemon crash.
- No kernel NBD disconnect.
- No filesystem corruption after unmount/remount.

### 6.4 Phase 1 Error Injection

Cases:

- Kill daemon while mounted: filesystem should receive IO errors, daemon cleanup path should run.
- Send SIGINT during active IO: daemon should disconnect NBD and free memory.
- Start with invalid `/dev/nbdX`: clear error message.
- Start without `modprobe nbd`: clear error message.

### 6.5 Phase 2 URMA Bring-up Test

Run two daemon instances or a daemon plus a test peer:

1. `urma_init()` succeeds.
2. Device lookup and EID selection succeed.
3. JFCE/JFC/JFR/Jetty creation succeeds.
4. Ramdisk memory registration succeeds.
5. Peer metadata exchange succeeds.
6. Remote segment import succeeds.
7. Remote jetty import succeeds.
8. RC/RS `urma_bind_jetty()` succeeds when applicable.

Expected logs:

```text
local eid
local uasid
local seg ubva
local seg len
local token
local jetty_id
remote seg import status
remote jetty import status
```

### 6.6 Phase 2 Control Plane Test

Cases:

| Case | Command | Expected result |
|------|---------|-----------------|
| URMA disabled | `RAMDISK_CTRL_URMA_TRANSFER` | `EOPNOTSUPP`; NBD read/write still succeeds |
| Query status | `RAMDISK_CTRL_QUERY_STATUS` | Reports NBD active, URMA enabled/disabled, peer count, queue depth |
| Peer connect | `RAMDISK_CTRL_PEER_CONNECT` with valid metadata | Peer context created, remote seg/jetty imported |
| Duplicate peer | Same `peer_id` twice | Deterministic replace or `EEXIST`, documented behavior |
| Peer disconnect | `RAMDISK_CTRL_PEER_DISCONNECT` | Imported resources released |
| Unknown peer transfer | `RAMDISK_CTRL_URMA_TRANSFER` with missing `peer_id` | `ENODEV` |
| Bad local range | Transfer beyond backend size | `ERANGE` |
| Bad remote range | Transfer beyond remote segment | `ERANGE` |

### 6.7 Phase 2 URMA Data Test

Cases:

| Case | Operation | Expected result |
|------|-----------|-----------------|
| Push 4 KiB | `URMA_OPC_WRITE` local ramdisk -> remote HBM | Remote buffer matches |
| Pull 4 KiB | `URMA_OPC_READ` remote HBM -> local ramdisk | Ramdisk range matches |
| Multiple outstanding WRs | READ/WRITE with unique `user_ctx` | Completion matches each request |
| Completion error | Inject bad token or invalid remote addr | Request fails safely |
| Disconnect peer | Stop peer during IO | Pending/inflight requests fail and cleanup completes |

### 6.8 Phase 2 Filesystem And URMA Coexistence Test

Cases:

| Case | Steps | Expected result |
|------|-------|-----------------|
| Filesystem works without URMA | Start daemon without `--enable-urma`, mkfs/mount/write/read file | Success |
| Filesystem works with URMA enabled | Start daemon with `--enable-urma`, mkfs/mount/write/read file | Success |
| URMA disabled does not break filesystem | Disable URMA at runtime, continue file read/write | Filesystem IO succeeds |
| Filesystem data pushed to HBM | Write file, `sync`, identify file block/range or use raw test range, transfer `RAMDISK_TO_HBM` | HBM data matches backend bytes |
| HBM pulled into backend raw range | Transfer `HBM_TO_RAMDISK` into raw offset, read `/dev/ramdisk` directly | Data matches |
| Mounted filesystem visibility after out-of-band write | Transfer into file data blocks, then `sync`, drop caches or remount, read file | Data matches only after explicit cache control |
| Unsafe metadata write guard | Attempt URMA write to protected metadata range if guard exists | Request rejected or test marked unsafe |

The key assertion is that URMA transfer is not discovered from ordinary NBD IO. It must be triggered only through the control plane.

### 6.9 Phase 2 Performance Smoke Test

Measure:

- Max outstanding WR count.
- JFC depth pressure.
- Average completion latency.
- READ/WRITE throughput for 4 KiB, 64 KiB, 1 MiB.

This is not the final performance test; it only validates that queue depth and completion matching are stable.

## 7. Implementation Notes

- Keep `ramdisk_backend` independent of NBD and URMA.
- Keep NBD request parsing independent of URMA.
- Add URMA as an optional module inside the userspace daemon.
- Do not leak segment token IDs in default logs; require verbose mode for sensitive transport metadata.
- `URMA_SEG_NOMAP` imported remote segments are valid for WRs but not CPU load/store.
- `complete_enable = 1` is required for request lifecycle tracking.
- Use `cr.user_ctx` as the completion key for data WRs.
- SEND/RECV receive buffers must be pre-posted.
- Always verify `cr.status`.

## 8. Open Questions

1. Should Phase 2 use polling or event-driven JFC completion by default?
2. Should the control plane stay TCP or move to an existing management channel?
3. What is the exact HBM peer metadata format required by the real device?
4. Should ramdisk memory use `posix_memalign`, hugepages, or UMDK-recommended allocation for best URMA registration behavior?
5. Should `/dev/ramdisk` be a symlink only, or should a udev rule be included?
