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
