# NVMe-oF Target RDMA Ramdisk Prototype

This repository contains a target-side prototype for mapping NVMe-oF IO requests to an RDMA-registered RAM backend.

The current implementation follows **scheme A**:

1. Allocate the whole ramdisk in target-owned memory.
2. Register the whole ramdisk as one MR.
3. Map every IO by `rdma_addr = mr_iova + lba * logical_block_size`.
4. Execute target-side RDMA operations against device C memory.

## Data Direction

NVMe directions are host-facing. RDMA directions are target-facing:

| Host request | Target RDMA operation | Data path |
| --- | --- | --- |
| NVMe read | RDMA WRITE | target ramdisk -> device C |
| NVMe write | RDMA READ | device C -> target ramdisk |

## Layout

- `skills/nvme-rdma-target/SKILL.md`: project skill for other Codex/code-agent sessions.
- `include/rdma_transport.h`: transport abstraction for RDMA connect, MR registration, and submit.
- `include/rdma_ramdisk.h`: ramdisk allocation, whole-MR registration, and LBA mapping API.
- `include/target_session.h`: target-side connect and IO submission API.
- `src/rdma_ramdisk.c`: scheme A implementation.
- `src/target_session.c`: NVMe-direction to RDMA-direction bridge.
- `src/mock_rdma.c`: in-memory RDMA mock.
- `tests/test_rdmadisk.c`: unit tests.

## Build

```sh
make
```

The output binary is:

```text
build/test_rdmadisk
```

## Run Tests

```sh
make test
```

Expected output:

```text
all tests passed
```

## Clean

```sh
make clean
```

## Production Integration Notes

The mock RDMA transport should be replaced by a kernel or userspace verbs implementation behind `struct rdma_transport_ops`.

The production NVMe-oF target extension should:

1. Receive device C RDMA connection information through the custom NVMe-oF control command.
2. Call `target_session_connect()`.
3. Resolve each IO command to `direction`, `lba`, `block_count`, `remote_addr`, `rkey`, and `remote_length`.
4. Call `target_session_submit_io()`.
5. Complete the NVMe command only after RDMA completion succeeds.

The important invariant is that the RAM backend is target-owned. Do not treat a generic Linux ramdisk block device as the source of truth for RDMA addresses.
