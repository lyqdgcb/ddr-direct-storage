---
name: nvme-rdma-target
description: Use this skill when working on the NVMe-oF target-side RDMA/URMA ramdisk project, especially code that maps NVMe LBAs to registered target RAM, establishes RDMA/URMA sessions to a host-side device, or performs RDMA/URMA read/write directly against the target memory backend.
---

# NVMe RDMA/URMA Target Ramdisk

## Project Summary

This project models the target-side data path for a system where:

- Machine A is the NVMe-oF host.
- Machine B is the NVMe-oF target.
- The target namespace backend is RAM owned by the target-side module.
- Host-side device C supports RDMA or URMA (Unified Bus).
- NVMe-oF carries control commands and IO requests.
- Target-side RDMA/URMA moves data directly between device C memory and the target ramdisk memory.

The code intentionally does not implement NVMe-oF command extensions or device C specifics. Treat those as outer protocol layers that call this project's target-side interfaces.

## Transport Options

This project supports two transport protocols:

| Transport | Directory | Description |
|-----------|-----------|-------------|
| **RDMA** | `kernel/` | Standard RDMA (ib_verbs), works on mainstream Linux |
| **URMA** | `kernel-urma/` | Huawei Unified Bus (UB), for OpenEuler/Huawei environments |

Choose based on your deployment environment:
- RDMA: Mellanox/Intel NICs, standard Linux kernels
- URMA: Huawei UB hardware, OpenEuler kernels with ubcore support

## Current Design

Use **scheme A** by default: allocate the whole target ramdisk, register the whole memory region once, and then translate every IO request into an offset inside that MR.

Core invariant:

```text
byte_offset = lba * logical_block_size
rdma_addr   = mr_base_iova + byte_offset
```

Do not depend on Linux `brd` or a generic block ramdisk to recover storage addresses. The memory backend must be owned by this project so LBA-to-memory mapping stays deterministic and RDMA-safe.

## Direction Mapping

NVMe operation names are from the host perspective. RDMA/URMA operations are from the target perspective:

```text
NVMe read  => target RDMA/URMA WRITE: ramdisk -> device C memory
NVMe write => target RDMA/URMA READ:  device C memory -> ramdisk
```

Return NVMe command completion only after the RDMA/URMA completion succeeds. Surface transport failures as NVMe command errors in the integration layer.

## RDMA vs URMA Comparison

### Concept Mapping

| RDMA Concept | URMA Equivalent | Description |
|--------------|-----------------|-------------|
| `ib_device` | `ubcore_device` | Transport device |
| `ib_pd` | `ubcore_token_id` | Protection domain / access credential |
| `ib_mr` | `ubcore_target_seg` | Memory region / segment |
| `lkey` | `token_id` | Local access key |
| `rkey` | `token_id` | Remote access key (same in URMA) |
| `iova` | `ubva.va` | Bus-accessible address |
| `ib_qp` | `ubcore_jetty` | Queue pair (JFS + JFR) |
| `ib_cq` | `ubcore_jfc` | Completion queue |
| `ib_sge` | `ubcore_sge` | Scatter-gather element |

### API Mapping

| RDMA API | URMA API |
|----------|----------|
| `ib_alloc_pd()` | `ubcore_alloc_token_id()` |
| `ib_reg_user_mr()` | `ubcore_register_seg()` |
| `ib_dereg_mr()` | `ubcore_unregister_seg()` |
| `ib_create_qp()` | `ubcore_create_jetty()` |
| `ib_create_cq()` | `ubcore_create_jfc()` |
| `ib_post_send()` | `ubcore_post_jetty_send_wr()` |
| `ib_poll_cq()` | `ubcore_poll_jfc()` |
| `ib_dma_map_single()` | Built into `ubcore_register_seg()` |

### Address Format

| Transport | Address Format |
|-----------|---------------|
| RDMA | `iova = mr->iova + offset` |
| URMA | `ubva = {eid, va}` where `va = seg->ubva.va + offset` |

### Header Files

| RDMA | URMA |
|------|------|
| `<rdma/ib_verbs.h>` | `<ub/urma/ubcore_types.h>` |
| `<rdma/rdma_cm.h>` | `<ub/urma/ubcore_uapi.h>` |
| `<rdma/uverbs_ioctl.h>` | `<ub/urma/ubcore_opcode.h>` |

### Kernel Module Sysfs

| RDMA (`/sys/block/rdma_blkdev/`) | URMA (`/sys/block/urma_blkdev/`) |
|----------------------------------|----------------------------------|
| `mr_info` | `seg_info` |
| `lkey` | `token_id` |
| `rkey` | (included in `seg_info`) |
| `iova` | `ubva` |
| `ramdisk_addr` | `ramdisk_addr` |
| `ramdisk_size` | `seg_len` |

### OpCode Mapping

| RDMA OpCode | URMA OpCode |
|-------------|-------------|
| `IB_WR_RDMA_WRITE` | `UBCORE_OPC_WRITE` |
| `IB_WR_RDMA_READ` | `UBCORE_OPC_READ` |
| `IB_WR_SEND` | `UBCORE_OPC_SEND` |
| `IB_WR_ATOMIC_CMP_AND_SWP` | `UBCORE_OPC_CAS` |
| `IB_WR_ATOMIC_FETCH_AND_ADD` | `UBCORE_OPC_FADD` |

### Environment Requirements

| Requirement | RDMA | URMA |
|-------------|------|------|
| Hardware | Mellanox/Intel RDMA NIC | Huawei UB hardware |
| Kernel | Mainline Linux | OpenEuler (ubcore enabled) |
| Driver | `ib_uverbs`, `ib_core` | `ubcore`, `uburma` |
| Package | `rdma-core` | `libuburma` |

## Code Layout

### User-space Library (shared)

- `include/rdma_ramdisk.h`: public ramdisk, MR, SGE, and IO API.
- `include/rdma_transport.h`: transport abstraction; production verbs/kernel code should implement this interface.
- `src/rdma_ramdisk.c`: scheme A memory allocation, whole-MR registration, LBA mapping, and IO planning.
- `src/target_session.c`: target-side session wrapper that maps control-plane connect info and IO commands to transport actions.
- `src/mock_rdma.c`: mock transport used by tests.
- `tests/test_rdmadisk.c`: unit tests for mapping and read/write direction.
- `examples/real_world_usage.c`: demonstration of address mapping in real deployment.
- `Makefile`: build and test entry points.

### Kernel Block Device Drivers

| Directory | Transport | Files |
|-----------|-----------|-------|
| `kernel/` | RDMA | `rdma_blkdev.c`, `Makefile`, `README.md` |
| `kernel-urma/` | URMA | `urma_blkdev.c`, `Makefile`, `README.md` |

Both kernel modules create a block device `/dev/{rdma|urma}_blkdev` backed by registered memory, export transport info via sysfs.

## Core Interfaces

Create a ramdisk:

```c
struct rdma_ramdisk *rdma_ramdisk_create(const struct rdma_ramdisk_config *cfg,
                                         struct rdma_transport_ops *ops,
                                         void *transport_ctx);
```

Map an IO:

```c
int rdma_ramdisk_map_lba(struct rdma_ramdisk *disk,
                         uint64_t lba,
                         uint32_t block_count,
                         struct rdma_ramdisk_mapping *out);
```

Execute target-side IO:

```c
int target_session_submit_io(struct target_session *session,
                             const struct target_io_request *req);
```

## Memory Rules

- The ramdisk base must remain alive and stable for the lifetime of the registered MR.
- The whole ramdisk is registered once during initialization.
- `mr_base_iova` may be zero in the mock implementation, but production code must store the actual IOVA returned or chosen during MR registration.
- Check every request for overflow and capacity bounds before computing RDMA addresses.
- Split cross-page logic can be added later, but scheme A may submit a contiguous SGE when the registered MR IOVA is contiguous over the ramdisk byte range.

## Development Workflow

1. Read this skill first.
2. Choose transport: RDMA (`kernel/`) or URMA (`kernel-urma/`) based on environment.
3. Keep transport-specific implementation behind abstraction layer.
4. Preserve the direction mapping exactly.
5. Add or update tests before changing address arithmetic.
6. Run `make test` after changes (for user-space code).
7. Build kernel module: `cd kernel{,-urma} && make`.

## Integration Notes

The production NVMe-oF target extension should:

- Call `target_session_connect()` after receiving device C RDMA/URMA endpoint and memory information.
- Call `target_session_submit_io()` when a custom NVMe command resolves to `lba`, `block_count`, `remote_addr`, `rkey/token_id`, and direction.
- Complete the NVMe command based on the returned transport status.

For kernel integration, replace the mock transport with transport-specific implementation. The core arithmetic and direction rules should remain unchanged.

## Kernel Block Device Modules

### RDMA Version (`kernel/rdma_blkdev.c`)

Creates block device backed by RDMA-registered memory:

```c
// Setup RDMA
ib_dev = ib_device_get_by_name("mlx5_0");
pd = ib_alloc_pd(ib_dev, IB_PD_UNSAFE_GLOBAL_RKEY);
mr = ib_get_dma_mr(pd, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
iova = ib_dma_map_single(ib_dev, ramdisk_addr, ramdisk_size, DMA_BIDIRECTIONAL);

// Sysfs exposes: lkey, rkey, iova, ramdisk_addr, ramdisk_size
```

### URMA Version (`kernel-urma/urma_blkdev.c`)

Creates block device backed by URMA-registered memory:

```c
// Setup URMA
ub_dev = ubcore_get_device_by_name("ub0");
token_id = ubcore_alloc_token_id(ub_dev, token_flag, NULL);
seg = ubcore_register_seg(ub_dev, &seg_cfg, NULL);

// seg->seg.ubva contains: eid + va (address)
// seg->seg.token_id is the access credential

// Sysfs exposes: ubva, token_id, seg_info, ramdisk_addr, ramdisk_size
```

### Usage Flow

1. **Load module**:
   ```bash
   # RDMA
   sudo insmod rdma_blkdev.ko device_size_mb=1024 ib_dev_name=mlx5_0
   
   # URMA
   sudo insmod urma_blkdev.ko device_size_mb=1024 ub_dev_name=ub0 eid_index=0
   ```

2. **Check device and transport info**:
   ```bash
   lsblk  # see /dev/rdma_blkdev or /dev/urma_blkdev
   cat /sys/block/rdma_blkdev/mr_info   # RDMA: lkey, rkey, iova
   cat /sys/block/urma_blkdev/seg_info  # URMA: ubva, token_id, eid
   ```

3. **Configure as NVMe-oF namespace**:
   ```bash
   echo -n /dev/rdma_blkdev > /sys/kernel/config/nvmet/.../device_path
   echo 1 > .../enable
   ```

4. **Get memory address for IO**:
   ```c
   // From NVMe command
   uint64_t lba = nvme_req->slba;
   
   // RDMA
   uint64_t rdma_addr = iova + lba * block_size;
   uint32_t rkey = mr_rkey;  // from sysfs
   
   // URMA
   uint64_t urma_addr = ubva + lba * block_size;
   uint32_t token = token_id;  // from sysfs
   ```

## Kernel Module Current Limitations

Both kernel modules currently provide:

| Feature | Status |
|---------|--------|
| Block device creation | ✅ Complete |
| Ramdisk memory allocation | ✅ Complete |
| Transport memory registration | ✅ Complete |
| Sysfs info export | ✅ Complete |
| Standard block IO (memcpy) | ✅ Complete |
| Transport data path (RDMA/URMA transfer) | ❌ Not implemented |
| NVMe-oF integration | ❌ Not implemented |
| Custom NVMe command parsing | ❌ Not implemented |

To implement full RDMA/URMA direct transfer, need to:
1. Intercept NVMe-oF target IO path
2. Parse custom NVMe command format (get remote_addr, rkey/token_id)
3. Build transport work request (RDMA WRITE/READ or URMA WRITE/READ)
4. Post to transport and poll completion
5. Complete NVMe command based on transport status

## Current Project Direction

The project has moved from a user-space mock prototype to a kernel-space implementation. The URMA path is now the primary target for the next integration step.

Current URMA progress:

| Feature | Status |
|---------|--------|
| Kernel ramdisk allocation | Complete in `kernel-urma/urma_blkdev.c` |
| Block device registration | Complete: exposes `/dev/urma_blkdev` |
| URMA device lookup | Present via `ubcore_get_device_by_name()` / netdev scan |
| Token allocation | Present via `ubcore_alloc_token_id()` |
| Whole ramdisk segment registration | Present via `ubcore_register_seg()` during module init |
| LBA to ramdisk memory mapping | Present for block memcpy path |
| LBA to UBVA mapping | Conceptually present: `seg->seg.ubva.va + lba * logical_block_size` |
| NVMe-oF target callable interface | Initial async API present in `kernel-urma/include/linux/urma_blkdev.h` |
| HBM peer EID / jetty / address import | Missing |
| Jetty READ/WRITE data path | Fail-safe stub present; real `ubcore_post_jetty_send_wr()` wiring still needed |

Important design decision: initialize URMA resources when the kernel ramdisk is created. The ramdisk memory must be registered as a URMA-accessible segment before NVMe-oF IO commands arrive, so the IO path only performs bounds checks, address translation, and jetty operations.

## NVMe-oF Target Integration Contract

The NVMe-oF target side will receive custom host commands carrying peer HBM transport metadata and IO location:

```text
peer_eid        HBM-side endpoint ID
peer_jetty      HBM-side jetty handle or import descriptor
hbm_addr        HBM memory address to read from or write to
lba             target ramdisk logical block address
block_count     number of logical blocks
direction       NVMe direction from host perspective
```

The NVMe-oF target extension should translate the custom command into a kernel call into this module. Do not make NVMe-oF parse this module's private ramdisk state. The module owns:

- `lba -> byte_offset`
- `byte_offset -> local ramdisk kernel address`
- `byte_offset -> local UBVA`
- target-side URMA token / registered segment
- jetty work request construction
- completion status mapping back to NVMe-oF

Recommended in-kernel API shape:

```c
enum urma_blkdev_io_dir {
    URMA_BLKDEV_IO_READ,   /* NVMe read: ramdisk -> HBM */
    URMA_BLKDEV_IO_WRITE,  /* NVMe write: HBM -> ramdisk */
};

struct urma_blkdev_peer {
    union ubcore_eid eid;
    struct ubcore_jetty *jetty;
};

struct urma_blkdev_io {
    enum urma_blkdev_io_dir dir;
    u64 lba;
    u32 block_count;
    u64 hbm_addr;
    u32 hbm_token_id;
};

int urma_blkdev_submit_nvmet_io(struct block_device *bdev,
                                const struct urma_blkdev_peer *peer,
                                const struct urma_blkdev_io *io,
                                urma_blkdev_done_fn done,
                                void *priv);
```

The exact peer jetty representation must be agreed with the NVMe-oF target changes. If NVMe-oF only has a serialized jetty descriptor, this module should import/resolve it before posting work requests.

## URMA Jetty Data Path

Direction mapping must stay host-semantic:

```text
NVMe read  => URMA WRITE: local ramdisk UBVA -> remote HBM address
NVMe write => URMA READ:  remote HBM address -> local ramdisk UBVA
```

For each request:

1. Validate module initialized and ramdisk segment registered.
2. Validate `block_count > 0`.
3. Compute `byte_offset = lba * logical_block_size`.
4. Reject overflow or ranges beyond `ramdisk_size`.
5. Compute local UBVA: `local_va = seg->seg.ubva.va + byte_offset`.
6. Build local SGE with this module's `seg` and token.
7. Build remote HBM SGE from `peer_eid`, `peer_jetty`, `hbm_addr`, and remote token/descriptor.
8. Post `UBCORE_OPC_WRITE` for NVMe read or `UBCORE_OPC_READ` for NVMe write.
9. Wait/poll completion according to agreed NVMe-oF completion model.
10. Return success/failure to NVMe-oF target so it can complete the command.

Do not fall back to the block layer memcpy path for direct HBM IO. The normal block path exists for Linux block-device compatibility; custom NVMe-oF direct-storage commands should call the URMA submit API directly.

## Queue Management Requirements

The NVMe-oF callable API must not synchronously post and busy-wait for every URMA request. NVMe-oF target can submit many commands concurrently, so this module needs explicit queue management.

The submit API should mean **accepted into this module**, not **IO completed**:

```c
typedef void (*urma_blkdev_done_fn)(void *priv, int status);

int urma_blkdev_submit_nvmet_io(struct block_device *bdev,
                                const struct urma_blkdev_peer *peer,
                                const struct urma_blkdev_io *io,
                                urma_blkdev_done_fn done,
                                void *priv);
```

`bdev` should be the NVMe-oF namespace block device for `/dev/urma_blkdev`. If the function returns `0`, ownership of the async request has moved to this module and completion must happen through `done(priv, status)`. If it returns an error, NVMe-oF target should complete the command immediately with an error.

Minimum internal request object:

```c
struct urma_blkdev_io_req {
    struct list_head list;
    struct urma_blkdev_device *dev;
    struct urma_blkdev_peer peer;
    struct urma_blkdev_io io;

    u64 byte_offset;
    u64 local_ubva;
    size_t len;

    u64 wr_id;
    urma_blkdev_done_fn done;
    void *priv;
    refcount_t ref;
};
```

Minimum device-side queue state:

```c
struct urma_blkdev_device {
    ...
    spinlock_t pending_lock;
    struct list_head pending_list;

    struct xarray inflight_reqs;   /* wr_id -> urma_blkdev_io_req */

    struct workqueue_struct *io_wq;
    struct work_struct submit_work;

    atomic_t pending;
    atomic_t inflight;
    u32 queue_depth;
    bool stopping;
};
```

When real jetty posting is wired, add a JFC polling thread or completion callback path that removes requests from `inflight_reqs` by `wr_id` and invokes `done(priv, status)`.

Required flow:

1. `urma_blkdev_submit_nvmet_io()` validates the request, computes `byte_offset`, `len`, and `local_ubva`, allocates `urma_blkdev_io_req`, appends it to `pending_list`, schedules `submit_work`, and returns.
2. `submit_work` drains pending requests while `inflight < queue_depth`, assigns `wr_id`, stores the request in `inflight_reqs`, builds the URMA work request, and posts it to the peer jetty.
3. If post fails, remove the request from `inflight_reqs`, call `done(priv, error)`, and release the request.
4. The completion path polls or receives JFC completions, looks up `wr_id` in `inflight_reqs`, removes the request, calls `done(priv, status)`, releases the request, and wakes `submit_work` to fill available queue slots.
5. Module unload or device teardown must set `stopping`, reject new submissions, fail all pending requests, and drain or fail all inflight requests before freeing ramdisk, segment, token, jetty, or completion resources.

Queue depth must be explicit. Add a module parameter such as:

```c
static unsigned int queue_depth = 1024;
module_param(queue_depth, uint, 0444);
```

Effective post depth should not exceed the smallest available limit:

```text
effective_depth = min(module_queue_depth, peer_jetty_sq_depth, jfc_capacity)
```

For the first implementation, a single global pending queue and global `wr_id -> request` map is acceptable. For production with multiple HBM peers or multiple jettys, evolve this into per-peer/per-jetty contexts:

```c
struct urma_blkdev_peer_ctx {
    union ubcore_eid eid;
    struct ubcore_jetty *jetty;

    spinlock_t pending_lock;
    struct list_head pending_list;

    struct xarray inflight_reqs;
    atomic_t inflight;
    struct work_struct submit_work;
};
```

Do not let NVMe-oF target own URMA queue depth, `wr_id` allocation, or completion matching. NVMe-oF should only submit requests and complete its command in the callback.
