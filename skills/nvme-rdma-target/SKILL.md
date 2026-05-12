---
name: nvme-rdma-target
description: Use this skill when working on the NVMe-oF target-side RDMA ramdisk project, especially code that maps NVMe LBAs to registered target RAM, establishes RDMA sessions to a host-side device, or performs RDMA read/write directly against the target memory backend.
---

# NVMe RDMA Target Ramdisk

## Project Summary

This project models the target-side data path for a system where:

- Machine A is the NVMe-oF host.
- Machine B is the NVMe-oF target.
- The target namespace backend is RAM owned by the target-side module.
- Host-side device C supports RDMA.
- NVMe-oF carries control commands and IO requests.
- Target-side RDMA moves data directly between device C memory and the target ramdisk memory.

The code intentionally does not implement NVMe-oF command extensions or device C specifics. Treat those as outer protocol layers that call this project's target-side interfaces.

## Current Design

Use **scheme A** by default: allocate the whole target ramdisk, register the whole memory region once, and then translate every IO request into an offset inside that MR.

Core invariant:

```text
byte_offset = lba * logical_block_size
rdma_addr   = mr_base_iova + byte_offset
```

Do not depend on Linux `brd` or a generic block ramdisk to recover storage addresses. The memory backend must be owned by this project so LBA-to-memory mapping stays deterministic and RDMA-safe.

## Direction Mapping

NVMe operation names are from the host perspective. RDMA operations are from the target perspective:

```text
NVMe read  => target RDMA WRITE: ramdisk -> device C memory
NVMe write => target RDMA READ:  device C memory -> ramdisk
```

Return NVMe command completion only after the RDMA completion succeeds. Surface RDMA failures as NVMe command errors in the integration layer.

## Code Layout

- `include/rdma_ramdisk.h`: public ramdisk, MR, SGE, and IO API.
- `include/rdma_transport.h`: RDMA transport abstraction; production verbs or kernel RDMA code should implement this interface.
- `src/rdma_ramdisk.c`: scheme A memory allocation, whole-MR registration, LBA mapping, and IO planning.
- `src/target_session.c`: target-side session wrapper that maps control-plane connect info and IO commands to RDMA actions.
- `src/mock_rdma.c`: mock RDMA transport used by tests.
- `tests/test_rdmadisk.c`: unit tests for mapping and read/write direction.
- `Makefile`: build and test entry points.

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
2. Keep the RDMA verbs/kernel-specific implementation behind `rdma_transport_ops`.
3. Preserve the direction mapping exactly.
4. Add or update tests before changing address arithmetic.
5. Run `make test` after changes.

## Integration Notes

The production NVMe-oF target extension should:

- Call `target_session_connect()` after receiving device C RDMA endpoint and memory information.
- Call `target_session_submit_io()` when a custom NVMe command resolves to `lba`, `block_count`, `remote_addr`, `rkey`, and direction.
- Complete the NVMe command based on the returned RDMA status.

For kernel integration, replace the mock transport with an implementation using `ib_pd`, `ib_mr`, QP/CQ setup, and RDMA work requests. The core arithmetic and direction rules should remain unchanged.
