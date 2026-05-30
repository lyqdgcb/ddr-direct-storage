# Userspace Ramdisk Development Design

本文档面向后续开发和联调人员，整理当前 `userspace-nbd` 目录下已经实现的用户态 ramdisk 代码结构、核心接口和关键流程，并补充 NVMe-oF target 通过 netlink 向用户态 daemon 下发 URMA 通信元数据的设计方案。

## 1. Overall Architecture

当前用户态 ramdisk 有两条路径，共享同一块 backend memory：

```text
Filesystem / ordinary block IO path:

application / filesystem
    -> /dev/ramdisk -> /dev/nbdX
    -> Linux nbd driver
    -> nbd-ramdisk daemon
    -> ramdisk_backend memory
```

```text
URMA special path:

control socket / future nvmet netlink bridge
    -> nbd-ramdisk daemon
    -> ramdisk_urma_mgr
    -> URMA peer lookup / queue / worker
    -> URMA READ/WRITE
    -> same ramdisk_backend memory <-> remote HBM
```

普通文件系统 IO 不携带 URMA 元数据，也不会自动触发 URMA。URMA 传输必须由显式控制面请求触发。

后续和 NVMe-oF target 对接后，普通路径仍然通过把 `/dev/nbdX` 挂成 nvmet namespace 来支持；URMA 特殊路径由 nvmet 内新增的 `nvmet_urma_bridge` 识别特殊 IO，再通过 Generic Netlink 把元数据传给用户态 daemon 处理。

## 2. Code Layout

主要目录是 `userspace-nbd/`。

核心文件：

- `src/nbd_ramdisk.c`
  - daemon 主程序。
  - 创建 backend memory。
  - 初始化 URMA manager。
  - 启动 Unix control socket server。
  - 通过 Linux NBD ioctl 把用户态内存暴露成 `/dev/nbdX`。
  - 处理 NBD READ/WRITE/FLUSH/TRIM/DISC 请求。

- `src/ramdisk_backend.c`
  - 管理 ramdisk backend memory。
  - 当前使用 `memalign(PAGE_SIZE, size)` 分配页对齐内存。
  - 提供 bounds check、全局 mutex、read/write/zero、base/size/block_size 查询接口。

- `src/ramdisk_urma.c`
  - 管理 URMA path。
  - 保存 peer table。
  - 维护 bounded request queue。
  - 启动 URMA worker thread。
  - 支持 mock URMA provider 和真实 UMDK URMA provider。
  - 真实路径负责 `urma_init`、context/JFC/JFR/Jetty 创建、ramdisk memory 注册、peer segment/jetty import、WR post 和 completion polling。

- `src/ramdisk_ctrl_server.c`
  - 当前 daemon 的 Unix domain socket 控制面。
  - `ramdiskctl`、测试程序、业务进程示例都通过它调用 URMA path。

- `src/ramdisk_ctrl_client.c`
  - 用户态客户端 helper。
  - 供 `ramdiskctl` 和 `examples/business_client_example.c` 复用。

- `include/ramdisk_ctrl.h`
  - 当前 Unix control socket wire protocol。
  - 定义 status、peer connect、peer disconnect、URMA transfer 等消息结构。

- `include/ramdisk_urma.h`
  - URMA manager、peer、request、config 等核心结构和 API。

## 3. Core Data Structures

### 3.1 Ramdisk Backend

`struct ramdisk_backend` 是整个系统的数据源：

```text
base        page-aligned userspace memory
size        ramdisk total size
block_size  logical block size
lock        global mutex
initialized lifecycle flag
```

关键接口：

```c
int ramdisk_backend_create(struct ramdisk_backend *backend,
                           uint64_t size, uint32_t block_size);
void ramdisk_backend_destroy(struct ramdisk_backend *backend);

int ramdisk_backend_read(struct ramdisk_backend *backend,
                         uint64_t offset, void *buf, uint32_t len);
int ramdisk_backend_write(struct ramdisk_backend *backend,
                          uint64_t offset, const void *buf, uint32_t len);
int ramdisk_backend_zero(struct ramdisk_backend *backend,
                         uint64_t offset, uint32_t len);

int ramdisk_backend_validate_range(const struct ramdisk_backend *backend,
                                   uint64_t offset, uint64_t len);
void *ramdisk_backend_base(struct ramdisk_backend *backend);
uint64_t ramdisk_backend_size(const struct ramdisk_backend *backend);
uint32_t ramdisk_backend_block_size(const struct ramdisk_backend *backend);
```

当前 backend 使用一个全局 mutex 保护 memory access。后续如果要模拟更真实的磁盘并发，可以在此基础上扩展 range lock 或 IO queue。

### 3.2 URMA Peer

`peer_id` 是 daemon 内部维护的远端通信对象句柄。它不是 EID，也不是 jetty 本身，而是用来索引一组远端 URMA 信息：

```text
peer_id
    -> eid
    -> uasid
    -> remote segment va / len / token
    -> remote jetty id
    -> imported segment / imported jetty
```

这样后续 IO 不需要每次都重复携带完整建链信息，只需要传：

```text
peer_id + local_offset + remote_hbm_addr + length + direction
```

### 3.3 URMA Request Queue

`struct ramdisk_urma_mgr` 内部维护一个有界环形队列：

```text
queue_depth
queue[head/tail]
pending
inflight
completed
failed
not_empty / not_full
worker thread
```

当前 public transfer API 是同步接口：

```c
int ramdisk_urma_transfer_sync(struct ramdisk_urma_mgr *mgr,
                               const struct ramdisk_ctrl_urma_transfer *xfer,
                               uint32_t timeout_ms);
```

这个接口会把请求放入队列，然后等待 worker 完成。后续对接 nvmet netlink 时，建议新增异步接口，例如：

```c
typedef void (*ramdisk_urma_done_fn)(void *priv, int status);

int ramdisk_urma_transfer_async(struct ramdisk_urma_mgr *mgr,
                                const struct ramdisk_ctrl_urma_transfer *xfer,
                                ramdisk_urma_done_fn done,
                                void *priv);
```

这样 netlink 接收线程不会因为等待单个 URMA IO 完成而被长时间阻塞。

## 4. Ramdisk Creation And Management Flow

### 4.1 Daemon Startup

当前 daemon 启动流程在 `src/nbd_ramdisk.c`：

```text
parse command line
    -> device path, size, block size, control sock, URMA options

ramdisk_backend_create()
    -> validate size
    -> validate block size
    -> memalign(PAGE_SIZE, size)
    -> memset zero
    -> init mutex

ramdisk_urma_mgr_init()
    -> allocate URMA queue
    -> init lock/cond
    -> optionally init real UMDK provider
    -> start URMA worker

ramdisk_ctrl_server_start()
    -> create Unix domain socket
    -> listen for ramdiskctl/business client requests

setup NBD
    -> open /dev/nbdX
    -> socketpair(AF_UNIX, SOCK_STREAM)
    -> NBD_SET_BLKSIZE
    -> NBD_SET_SIZE
    -> NBD_SET_FLAGS
    -> NBD_SET_SOCK
    -> start NBD_DO_IT thread

serve_nbd()
    -> process block requests from Linux nbd driver
```

### 4.2 Ordinary NBD IO

NBD request handling is currently serialized in one serve loop:

```text
read nbd request header
    -> validate magic/type/offset/length
    -> READ:  ramdisk_backend_read(), send reply + payload
    -> WRITE: read payload, ramdisk_backend_write(), send reply
    -> FLUSH: send success
    -> TRIM:  ramdisk_backend_zero(), send reply
    -> DISC:  cleanup and exit loop
```

当前 NBD path 没有独立 worker pool，也没有模拟磁盘队列和延迟。已有 TODO 计划后续扩展：

```text
NBD request reader
    -> bounded block_io_queue
    -> worker pool
    -> backend memory access
    -> NBD reply
```

### 4.3 Shutdown

收到信号或 NBD disconnect 时：

```text
NBD_DISCONNECT
    -> stop NBD_DO_IT
    -> CLEAR_QUE / CLEAR_SOCK
    -> stop control server
    -> destroy URMA manager
    -> destroy backend memory
```

异常路径必须保证不泄露 fd/thread/memory，也不能因为用户态错误影响内核稳定性。

## 5. URMA Link Setup And Transfer Flow

### 5.1 URMA Manager Initialization

Mock 模式：

```text
allocate queue
init locks/conds
start worker
```

Real UMDK 模式：

```text
urma_init()
    -> select device by local EID
    -> urma_query_device()
    -> select eid_index
    -> urma_create_context()
    -> urma_create_jfce()
    -> urma_create_jfc()
    -> urma_create_jfr()
    -> urma_create_jetty()
    -> urma_register_seg(ramdisk_backend_base(), ramdisk_backend_size())
    -> start worker
```

关键点是：真实 URMA 注册的 memory 就是 backend memory 本身，不能创建第二块 shadow buffer。

### 5.2 Peer Connect

当前通过 `RAMDISK_CTRL_PEER_CONNECT` 进入：

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

流程：

```text
validate peer_id / seg_len
    -> build ramdisk_urma_peer
    -> mock mode: allocate mock_hbm
    -> real mode:
        -> fill urma_seg_t with peer EID/UASID/VA/len/token
        -> urma_import_seg()
        -> fill remote jetty info
        -> urma_import_jetty()
    -> check URMA path enabled
    -> insert into peer table
```

### 5.3 URMA Transfer

当前 transfer 请求：

```c
struct ramdisk_ctrl_urma_transfer {
    uint64_t peer_id;
    uint64_t request_id;
    uint64_t local_offset;
    uint64_t remote_hbm_addr;
    uint32_t length;
    uint32_t direction;
};
```

方向定义从 daemon 视角描述：

```text
RAMDISK_TO_HBM
    local ramdisk memory -> remote HBM
    real URMA opcode: URMA_OPC_WRITE

HBM_TO_RAMDISK
    remote HBM -> local ramdisk memory
    real URMA opcode: URMA_OPC_READ
```

处理流程：

```text
ramdisk_urma_transfer_sync()
    -> validate enabled
    -> validate direction
    -> validate local range in backend
    -> validate block-size alignment
    -> lookup peer_id
    -> validate remote_hbm_addr range in peer segment
    -> check queue not full
    -> enqueue request
    -> wait done_cond

URMA worker
    -> dequeue
    -> validate again
    -> mock mode: memcpy between backend memory and peer mock_hbm
    -> real mode:
        -> local SGE points to backend base + local_offset
        -> remote SGE points to imported peer segment + remote offset
        -> build urma_jfs_wr_t
        -> post urma_post_jetty_send_wr()
        -> poll urma_poll_jfc()
        -> match completion by request_id/user_ctx
    -> update completed/failed
    -> wake waiting requester
```

注意：当前 worker 中存在把 `remote_hbm_addr` 改成 `peer->seg_va` 的调试代码痕迹。后续联调前应确认是否需要保留；正式逻辑应使用请求携带的 `remote_hbm_addr`，否则会丢失远端 HBM 偏移。

## 6. Current Unix Control Socket Flow

Unix control socket 是当前已经实现的用户态控制面，用于调试和业务进程示例。

```text
ramdiskctl / business_client_example
    -> ramdisk_ctrl_client.c
    -> Unix socket
    -> ramdisk_ctrl_server.c
    -> ramdisk_urma_mgr
```

支持 opcode：

```text
RAMDISK_CTRL_QUERY_STATUS
RAMDISK_CTRL_URMA_ENABLE
RAMDISK_CTRL_URMA_DISABLE
RAMDISK_CTRL_PEER_CONNECT
RAMDISK_CTRL_PEER_DISCONNECT
RAMDISK_CTRL_URMA_TRANSFER
```

这个控制面后续应继续保留，用于：

- 本地调试。
- 无 nvmet 环境下验证 URMA path。
- 业务进程直接调用 daemon。
- netlink path 的对照测试。

## 7. NVMe-oF Target Netlink Bridge Design

### 7.1 Goal

新增 `nvmet_urma_bridge`，使 nvmet target 在收到 URMA 特殊 IO 后，不在内核里直接执行 URMA，而是把必要元数据通过 Generic Netlink 发给用户态 daemon。

数据本体不经过 netlink。netlink 只传控制和元数据：

```text
eid / uasid / jetty / remote segment / lba / length / remote_hbm_addr / direction
```

真实数据仍然由 daemon 通过 URMA READ/WRITE 在 ramdisk memory 和 HBM 之间搬运。

### 7.2 Module Roles

```text
nvmet core
    -> receive NVMe command
    -> dispatch ordinary IO to namespace block device
    -> dispatch URMA special IO to nvmet_urma_bridge

nvmet_urma_bridge
    -> parse URMA command metadata
    -> map LBA to local_offset
    -> allocate request_id
    -> keep request_id -> nvmet_req inflight entry
    -> send Generic Netlink IO_REQUEST to daemon
    -> receive IO_COMPLETE from daemon
    -> call nvmet_req_complete()

nbd-ramdisk daemon
    -> keep NBD backend online
    -> listen Generic Netlink messages
    -> execute peer connect / transfer through ramdisk_urma_mgr
    -> send completion status back to kernel
```

### 7.3 Generic Netlink Family

Recommended family:

```text
family name: nvmet_urma
kernel module/file: nvmet_urma_bridge
userspace component: ramdisk_netlink_agent
```

Recommended commands:

```text
NVMET_URMA_CMD_REGISTER_DAEMON
NVMET_URMA_CMD_UNREGISTER_DAEMON
NVMET_URMA_CMD_HEARTBEAT
NVMET_URMA_CMD_PEER_CONNECT
NVMET_URMA_CMD_PEER_DISCONNECT
NVMET_URMA_CMD_IO_REQUEST
NVMET_URMA_CMD_IO_COMPLETE
NVMET_URMA_CMD_STATUS
```

Kernel side should not run a manual receive loop. Generic Netlink works by registering family ops and command callbacks. User space daemon runs a long-lived receive loop.

### 7.4 Daemon Registration

Daemon must stay running. On startup, after backend and URMA manager are initialized, it registers to `nvmet_urma_bridge`:

```text
daemon
    -> open Generic Netlink socket
    -> resolve family "nvmet_urma"
    -> send REGISTER_DAEMON
    -> kernel records daemon netlink portid
    -> kernel marks daemon registered
```

Register attributes should include:

```text
version
instance name
device_path, e.g. /dev/nbd0
backend_size
block_size
queue_depth
capability flags
```

Kernel state:

```text
registered
daemon_portid
backend_size
block_size
queue_depth
last_heartbeat
inflight table
```

Only one daemon is required for the first version. If multiple ramdisk instances are needed later, extend registration with `backend_id` or namespace mapping.

### 7.5 IO Request Flow

For URMA special IO:

```text
host sends NVMe URMA special command
    -> nvmet receives request
    -> nvmet_urma_bridge parses metadata
    -> local_offset = lba * namespace_block_size
    -> length = block_count * namespace_block_size
    -> request_id allocated
    -> request_id -> nvmet_req inserted into inflight table
    -> IO_REQUEST sent to daemon over Generic Netlink

daemon receives IO_REQUEST
    -> validate URMA enabled
    -> validate namespace/backend match
    -> validate local_offset/length
    -> lookup peer_id or build peer metadata
    -> enqueue URMA transfer
    -> worker executes URMA
    -> daemon sends IO_COMPLETE(request_id, status)

kernel receives IO_COMPLETE
    -> lookup request_id
    -> remove inflight entry
    -> translate status to NVMe status
    -> nvmet_req_complete()
```

This must be asynchronous. nvmet should not wait synchronously in the command receive path for userspace daemon completion.

### 7.6 Peer Metadata Policy

There are two possible metadata modes:

```text
Mode A: IO carries full peer metadata
    + easiest for initial bring-up
    - repeats EID/jetty/segment on every IO

Mode B: peer_connect first, IO carries peer_id
    + cleaner and closer to connection/session model
    + avoids repeated import work
    - needs peer lifecycle management
```

Recommended implementation:

1. First bring-up may support full metadata in `IO_REQUEST`.
2. Stable path should use `PEER_CONNECT(peer_id, metadata)` and `IO_REQUEST(peer_id, offset, remote_hbm_addr, length, direction)`.

### 7.7 Failure Handling

Kernel side must handle:

- Daemon not registered: fail URMA IO immediately.
- Netlink send failure: remove inflight entry and complete request with error.
- Daemon unregister: fail all inflight requests.
- Heartbeat timeout: mark daemon dead and fail all inflight requests.
- Request timeout: complete request with error and ignore later duplicate completion.
- Unknown `request_id` completion: log and drop.
- Duplicate completion: log and drop.
- Namespace/backend mismatch: reject request.

Daemon side must handle:

- URMA disabled: return error completion.
- Unknown peer: return error completion.
- Local range overflow: return error completion.
- Remote range overflow: return error completion.
- Unaligned URMA request: return error completion.
- Queue full: return `-EBUSY` or wait only up to request timeout.
- URMA post/poll failure: return error completion.

All error paths must be logged with enough metadata:

```text
request_id, peer_id, nsid/backend_id, lba, local_offset,
remote_hbm_addr, length, direction, errno/status
```

## 8. Interface Mapping Between nvmet And Daemon

NVMe operation names are from host perspective. URMA direction names are from daemon perspective.

```text
Host reads from NVMe namespace:
    target needs to send ramdisk data to remote HBM
    daemon direction = RAMDISK_TO_HBM
    URMA opcode = WRITE

Host writes to NVMe namespace:
    target needs to fetch data from remote HBM into ramdisk
    daemon direction = HBM_TO_RAMDISK
    URMA opcode = READ
```

LBA mapping:

```text
local_offset = lba * block_size
length = block_count * block_size
```

The daemon should always validate that:

```text
local_offset + length <= backend_size
remote_hbm_addr belongs to peer segment
length is non-zero
URMA alignment rules are satisfied
```

## 9. Build And Run

Mock/default build:

```sh
cd userspace-nbd
make
make test
```

Real URMA build:

```sh
cd userspace-nbd
make HAVE_URMA=1 \
  URMA_LIBDIR=/path/to/liburma \
  URMA_LIBS="-lurma"
```

Run daemon:

```sh
sudo modprobe nbd max_part=8
sudo ./build/nbd-ramdisk \
  --device /dev/nbd0 \
  --size 128M \
  --control-sock /tmp/nbd-ramdisk-control.sock \
  --enable-urma
```

Expose as ramdisk:

```sh
sudo ln -sf /dev/nbd0 /dev/ramdisk
sudo mkfs.ext4 -F /dev/ramdisk
sudo mount /dev/ramdisk /mnt/ramdisk
```

Use `/dev/nbd0` or `/dev/ramdisk` as nvmet namespace backend:

```sh
echo -n /dev/nbd0 > /sys/kernel/config/nvmet/subsystems/<subsys>/namespaces/1/device_path
echo 1 > /sys/kernel/config/nvmet/subsystems/<subsys>/namespaces/1/enable
```

## 10. Test Plan

Existing tests:

```text
test_backend
    backend create/read/write/zero/bounds

test_urma_queue
    mock peer connect
    RAMDISK_TO_HBM
    HBM_TO_RAMDISK
    alignment/range rejection
    parallel transfer submissions

test_ctrl_server
    Unix control socket status/enable/peer-connect
```

Netlink bridge tests to add:

```text
userspace unit tests:
    encode/decode REGISTER_DAEMON
    encode/decode IO_REQUEST/IO_COMPLETE
    map IO_REQUEST to ramdisk_ctrl_urma_transfer
    reject invalid length, offset, peer, direction

kernel tests or debug hooks:
    daemon register/unregister
    inflight request insert/remove
    timeout cleanup
    duplicate completion handling
    daemon death cleanup

integration tests:
    ordinary filesystem IO through /dev/nbdX still works
    nvmet namespace backed by /dev/nbdX works for normal IO
    URMA special read maps to RAMDISK_TO_HBM
    URMA special write maps to HBM_TO_RAMDISK
    concurrent URMA IO completes with correct request_id
    daemon restart causes safe failure and recovery
```

## 11. Development Notes

- Keep ordinary NBD path and URMA special path explicitly separated.
- Netlink must only carry metadata, never bulk data.
- URMA completion must be matched by `request_id`.
- Do not complete nvmet requests until daemon reports URMA completion or kernel timeout fires.
- Keep Unix control socket as a debug and test interface even after netlink is added.
- For real URMA, backend memory must remain stable and registered for the daemon lifetime.
- Do not assume filesystem page cache coherence after out-of-band URMA writes. Tests mixing filesystem IO and URMA IO should use explicit sync/remount/drop-cache or direct IO where appropriate.
