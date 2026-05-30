# Userspace Ramdisk TODO

## Make NBD Path Behave More Like A Real Disk

Current NBD filesystem path is a simple serial request loop:

```text
NBD socket -> parse one request -> backend read/write/zero -> reply
```

For a ramdisk intended to simulate a disk, add an explicit block-device IO queue model.

### Target Design

```text
NBD request reader
    -> ramdisk_io_request
    -> bounded block_io_queue
    -> worker pool
    -> backend memory access
    -> NBD reply
```

### Required Features

- Add configurable NBD queue depth.
  - Example option: `--nbd-queue-depth <n>`
  - Queue full should apply backpressure instead of dropping requests.
- Add configurable NBD worker count.
  - Example option: `--nbd-workers <n>`
  - Start conservative: default `1`, allow more for disk simulation.
- Represent each filesystem IO as an internal request object:
  - NBD handle
  - command type: read/write/trim/flush/disconnect
  - offset
  - length
  - payload buffer for writes
  - completion status
- Preserve correct NBD reply ordering where required by protocol behavior.
  - If out-of-order replies are allowed for the current NBD mode, document it.
  - If uncertain, keep replies serialized through a completion/reply thread.
- Expose queue status through control-plane status:
  - NBD queue depth
  - NBD pending
  - NBD inflight
  - NBD completed
  - NBD failed

### Disk-Like Latency Simulation

Add configurable latency injection to make the device behave less like zero-latency memory.

Initial options:

```text
--read-latency-us <n>
--write-latency-us <n>
--flush-latency-us <n>
--trim-latency-us <n>
--latency-jitter-us <n>
```

Rules:

- Apply latency in worker threads before completing the request.
- Keep latency disabled by default.
- Jitter should be bounded and deterministic enough for tests when a seed is provided.
- Consider adding `--latency-seed <n>` if tests need reproducible timing.

### Later Disk Simulation Features

- Maximum IO size.
- Logical/physical block size distinction.
- Optional 4 KiB alignment enforcement mode.
- Simple scheduling policy:
  - FIFO first.
  - Later: read-priority, offset-order, or deadline-like scheduling.
- Range locks instead of global backend mutex.
- Per-queue statistics and latency histogram.
- Optional error injection:
  - read error rate
  - write error rate
  - timeout injection
  - media error over configured ranges

### Test Plan

- Unit test queue push/pop, full queue, shutdown drain.
- Unit test worker pool with parallel reads/writes.
- Unit test latency options with coarse timing bounds.
- Integration test `mkfs/mount/read/write` still works.
- Stress test with `fio` and multiple jobs.

### Priority

Implement after the current URMA bring-up path is stable enough, but before using this userspace ramdisk for any disk-like performance or concurrency evaluation.
