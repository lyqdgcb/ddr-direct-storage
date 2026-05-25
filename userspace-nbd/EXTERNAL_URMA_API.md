# 外部模块 URMA 路径接口说明

本文档说明外部用户态程序如何把自己的 URMA 资源接入 `nbd-ramdisk`，并通过控制接口触发 ramdisk 后端内存和外部模块注册内存之间的数据传输。

控制面公开接口定义在：

- `include/ramdisk_ctrl.h`
- `include/ramdisk_ctrl_client.h`
- `src/ramdisk_ctrl_client.c`

`ramdiskctl` 只是调试命令行工具。正式外部模块建议链接 `libnbd_ramdisk_ctrl.so`，通过 `ramdisk_ctrl_client.h` 中的 API 调用 control socket。

## 整体模型

系统里有两个进程：

1. `nbd-ramdisk`
   - 拥有 NBD 块设备。
   - 拥有本地 ramdisk backend 内存。
   - 使用 `--real-urma` 启动时，会把 backend 内存注册成自己的本地 URMA segment。
   - 通过 control socket 接收外部模块的 peer 信息，并 import 外部 segment 和 jetty。

2. 外部模块
   - 自己创建真实 URMA context、JFC、JFR、Jetty 和 registered segment。
   - 只把 URMA metadata 传给 `nbd-ramdisk`，包括 EID、UASID、segment VA、segment 长度、token id、Jetty id。
   - 通过 control socket 调用 connect、transfer、disconnect。

daemon 不会直接拿到外部模块的普通进程指针。外部模块传递的是可被 URMA import 的地址描述。

## 启动 daemon

示例：

```sh
sudo ./build/nbd-ramdisk \
  --device /dev/nbd0 \
  --size 128M \
  --control-sock /tmp/nbd-ramdisk-control.sock \
  --enable-urma \
  --real-urma \
  --urma-eid <ramdisk-local-eid-hex> \
  --urma-trans-mode rc \
  --urma-tp-type rtp
```

`--urma-eid` 是 daemon 侧本地 EID，格式是 16 字节 EID 的 32 个十六进制字符，允许带 `:` 或 `-` 分隔符。

外部模块必须使用和 daemon 不同的 EID。如果两者在同一张设备上运行，需要选择不同的 EID index 创建各自的 URMA context。

## 外部模块如何链接

当前 Makefile 会产出控制面 client so：

```text
build/lib/libnbd_ramdisk_ctrl.so
```

外部 C/C++ 模块包含头文件：

```c
#include "ramdisk_ctrl_client.h"
```

链接示例：

```sh
cc -I/path/to/userspace-nbd/include \
   your_module.c \
   -L/path/to/userspace-nbd/build/lib \
   -lnbd_ramdisk_ctrl \
   -Wl,-rpath,/path/to/userspace-nbd/build/lib \
   -o your_module
```

如果外部模块本身还要创建 URMA 资源，需要同时包含 UMDK 头文件并链接 `liburma`。

## 外部模块需要创建的 URMA 资源

在调用 `peer-connect` 之前，外部模块需要创建真实 URMA 资源，不能手工伪造数字。

典型流程：

1. `urma_init`
2. `urma_get_device_by_name` 或 `urma_get_device_by_eid`
3. `urma_create_context`
4. `urma_create_jfce`
5. `urma_create_jfc`
6. `urma_create_jfr`
7. `urma_create_jetty`
8. 分配页对齐的外部内存。
9. `urma_register_seg`

然后用真实 URMA 对象填充：

```c
struct ramdisk_ctrl_peer_connect peer = {0};

peer.peer_id = your_stable_peer_id;
memcpy(peer.eid, local_tseg->seg.ubva.eid.raw, sizeof(peer.eid));
peer.uasid = urma_ctx->uasid;
peer.seg_va = local_tseg->seg.ubva.va;
peer.seg_len = local_tseg->seg.len;
peer.seg_token_id = local_tseg->seg.token_id;
peer.jetty_id = jetty->jetty_id.id;
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `peer_id` | 外部模块指定的非 0 逻辑 ID，后续 transfer 和 disconnect 都用它查找 peer。 |
| `eid[16]` | 外部模块 URMA EID 的 raw bytes，网络字节序。 |
| `uasid` | 外部模块 `urma_context_t->uasid`。 |
| `seg_va` | 外部 registered segment 的 `local_tseg->seg.ubva.va`。这不是 daemon 进程里的普通指针。 |
| `seg_len` | 外部 registered segment 的长度。 |
| `seg_token_id` | 外部 registered segment 的 token id。 |
| `jetty_id` | 外部 Jetty id，即 `jetty->jetty_id.id`。 |

注意：`eid`、`seg_va`、`seg_token_id`、`jetty_id` 都必须来自真实 URMA 资源，不能随便填数字。

## Connect 接口

先启用 URMA 路径，再连接外部 peer：

```c
const char *sock = "/tmp/nbd-ramdisk-control.sock";
int rc;

rc = ramdisk_ctrl_enable_urma(sock);
if (rc != 0)
    return rc;

rc = ramdisk_ctrl_peer_connect(sock, &peer);
if (rc == -EEXIST) {
    /* peer_id 已经存在。只有确认 metadata 完全一致时才建议复用。 */
    rc = 0;
}
if (rc != 0)
    return rc;
```

`peer-connect` 成功后，`nbd-ramdisk` 内部会执行：

- `urma_import_seg`：import 外部模块的 registered segment。
- `urma_import_jetty`：import 外部模块的 Jetty。
- 如果 transport mode 是 RC，还会执行 Jetty bind。

外部模块必须保证自己的 URMA context、Jetty、registered segment 在 `peer-disconnect` 完成之前一直有效。

## 传输接口

接口：

```c
int ramdisk_ctrl_urma_transfer(
    const char *sock_path,
    const struct ramdisk_ctrl_urma_transfer *xfer);
```

请求结构：

```c
struct ramdisk_ctrl_urma_transfer xfer = {0};

xfer.peer_id = peer.peer_id;
xfer.request_id = request_id;
xfer.local_offset = ramdisk_offset;
xfer.remote_hbm_addr = peer.seg_va + remote_offset;
xfer.length = length;
xfer.direction = RAMDISK_TO_HBM; /* 或 HBM_TO_RAMDISK */

rc = ramdisk_ctrl_urma_transfer(sock, &xfer);
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `peer_id` | 已经 connect 成功的 peer id。必须和 `peer-connect` 时传入的值一致。 |
| `request_id` | 单次传输请求 ID，会写入 URMA WR 的 `user_ctx`，建议每次请求唯一递增。 |
| `local_offset` | daemon 本地 ramdisk backend 内的偏移，不是指针。 |
| `remote_hbm_addr` | 外部 registered segment 内的地址，通常是 `peer.seg_va + remote_offset`。 |
| `length` | 传输长度，必须非 0。 |
| `direction` | 传输方向，取 `RAMDISK_TO_HBM` 或 `HBM_TO_RAMDISK`。 |

daemon 会校验：

- URMA path 已启用。
- `peer_id` 对应的 peer 已连接。
- `local_offset + length` 在 ramdisk backend 范围内。
- `remote_hbm_addr + length` 在外部 registered segment 范围内。
- `local_offset`、`remote_hbm_addr`、`length` 都按 ramdisk block size 对齐。

该接口是同步接口：函数返回时，daemon worker 已经 post URMA WR 并 poll 到 completion，或者已经返回错误。

## 方向语义

| direction | URMA opcode | daemon 本地侧 | 外部模块侧 |
| --- | --- | --- | --- |
| `RAMDISK_TO_HBM` | `URMA_OPC_WRITE` | 源：ramdisk backend | 目的：外部 segment |
| `HBM_TO_RAMDISK` | `URMA_OPC_READ` | 目的：ramdisk backend | 源：外部 segment |

从外部模块角度理解：

- `RAMDISK_TO_HBM`：把 ramdisk 中的数据传到外部模块注册的内存。
- `HBM_TO_RAMDISK`：把外部模块注册内存中的数据写回 ramdisk。

## peer_id 和 request_id 规则

`peer_id` 不是随意值：

- 必须是 `ramdisk_ctrl_peer_connect.peer_id` 中已经 connect 成功的值。
- 不能为 `0`。
- 同一个 daemon 内不能重复连接相同 `peer_id`，重复连接会返回 `-EEXIST`。
- 它只是 control plane 逻辑 ID，不要求等于 EID、Jetty id 或 token。

推荐生成方式：

```c
peer.peer_id = ((uint64_t)eid_index << 32) | jetty->jetty_id.id;
if (peer.peer_id == 0)
    peer.peer_id = 1;
```

`request_id` 由调用方生成：

- 每次 transfer 建议唯一。
- 会写入 WR 的 `user_ctx`。
- completion 返回时 daemon 会用它匹配请求。

推荐生成方式：

```c
static uint32_t seq;
uint64_t request_id = ((uint64_t)getpid() << 32) | ++seq;
```

## 状态查询

接口：

```c
struct ramdisk_ctrl_status status;

rc = ramdisk_ctrl_query_status(sock, &status);
if (rc == 0) {
    printf("size=%llu block=%u urma=%u peers=%u pending=%u inflight=%u completed=%llu failed=%llu\n",
           (unsigned long long)status.size,
           status.block_size,
           status.urma_enabled,
           status.peer_count,
           status.pending,
           status.inflight,
           (unsigned long long)status.completed,
           (unsigned long long)status.failed);
}
```

常用字段：

| 字段 | 含义 |
| --- | --- |
| `size` | ramdisk backend 总大小。 |
| `block_size` | 块大小，也是 URMA transfer 的对齐粒度。 |
| `urma_enabled` | URMA 路径是否启用。 |
| `peer_count` | 当前已连接 peer 数。 |
| `pending` | URMA worker 队列中等待执行的请求数。 |
| `inflight` | 当前正在执行的请求数。 |
| `completed` | 成功完成的请求数。 |
| `failed` | 失败请求数。 |

## Disconnect 和资源释放

外部模块销毁 URMA 资源之前，必须先通知 daemon 断开 peer：

```c
rc = ramdisk_ctrl_peer_disconnect(sock, peer.peer_id);

/* disconnect 成功后，再销毁外部 URMA 资源 */
urma_unregister_seg(local_tseg);
urma_delete_jetty(jetty);
urma_delete_jfr(jfr);
urma_delete_jfc(jfc);
urma_delete_jfce(jfce);
urma_delete_context(urma_ctx);
urma_uninit();
```

不要在 transfer 进行中 unregister segment 或 delete Jetty。

## 给外部模块传递接口的方式

推荐方式是提供 so 和头文件：

```text
userspace-nbd/include/ramdisk_ctrl.h
userspace-nbd/include/ramdisk_ctrl_client.h
userspace-nbd/build/lib/libnbd_ramdisk_ctrl.so
```

外部模块只需要调用：

```c
ramdisk_ctrl_query_status()
ramdisk_ctrl_enable_urma()
ramdisk_ctrl_peer_connect()
ramdisk_ctrl_urma_transfer()
ramdisk_ctrl_peer_disconnect()
ramdisk_ctrl_disable_urma()
```

对于非 C/C++ 模块，也可以直接实现 Unix domain socket 协议：发送 `struct ramdisk_ctrl_hdr` 和 payload，读取 `struct ramdisk_ctrl_resp` 和可选响应 payload。

注意：当前协议结构体是本地 C ABI 格式。如果要跨语言、跨架构或长期稳定发布，建议后续再定义显式 packed 的 wire format。

## 最小调用流程

```text
外部模块：
  创建真实 URMA context/JFC/JFR/Jetty
  分配并注册外部 segment
  从真实 URMA 对象中填充 ramdisk_ctrl_peer_connect

control socket：
  query_status
  enable_urma
  peer_connect
  transfer RAMDISK_TO_HBM 或 HBM_TO_RAMDISK
  peer_disconnect

外部模块：
  销毁 URMA 资源
```

完整示例可参考：

```text
examples/business_client_example.c
```

