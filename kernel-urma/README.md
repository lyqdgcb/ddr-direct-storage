# URMA Ramdisk Block Device Driver

创建一个 URMA-enabled ramdisk 块设备，可作为 NVMe-oF target namespace 后端使用。

## 编译

```bash
cd kernel-urma
make
```

**依赖**：
- URMA 内核头文件：`include/ub/urma/ubcore_types.h`, `ubcore_uapi.h`
- OpenEuler 内核源码（包含 URMA 支持）

## 加载模块

```bash
# 加载模块，指定大小和 URMA 设备
sudo insmod urma_blkdev.ko device_size_mb=1024 ub_dev_name=ub0 eid_index=0

# 或不指定 URMA 设备（纯内存模式）
sudo insmod urma_blkdev.ko device_size_mb=256
```

## 模块参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `device_size_mb` | 256 | 设备大小（MB） |
| `logical_block_size` | 512 | 逻辑块大小 |
| `ub_dev_name` | NULL | URMA 设备名（如 ub0） |
| `eid_index` | 0 | EID 索引 |

## 查看设备信息

```bash
# 查看块设备
lsblk

# 查看 URMA segment 信息
cat /sys/block/urma_blkdev/seg_info

# 输出示例：
# ubva_eid=xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx
# ubva_va=0x1000000000
# len=1073741824
# token_id=0x12345678
# addr=0xffff888012340000

# 查看单个属性
cat /sys/block/urma_blkdev/ubva
cat /sys/block/urma_blkdev/token_id
cat /sys/block/urma_blkdev/seg_len
cat /sys/block/urma_blkdev/ub_dev_name
```

## URMA vs RDMA 对比

| RDMA | URMA | 说明 |
|------|------|------|
| `ib_mr` | `ubcore_target_seg` | 内存注册 |
| `lkey/rkey` | `token_id` | 访问凭证 |
| `iova` | `ubva.va` | URMA 地址 |
| `ib_qp` | `ubcore_jetty` | 传输队列 |
| `ib_cq` | `ubcore_jfc` | 完成队列 |
| `ib_post_send` | `ubcore_post_jetty_send_wr` | 发送请求 |
| `ib_poll_cq` | `ubcore_poll_jfc` | 轮询完成 |
| `ib_reg_user_mr` | `ubcore_register_seg` | 注册内存 |
| `ib_alloc_pd` | `ubcore_alloc_token_id` | 分配访问凭证 |

## URMA 核心数据结构

### Segment（内存段）

```c
struct ubcore_seg {
    struct ubcore_ubva ubva;  // UBVA 地址 (eid + va)
    uint64_t len;             // 长度
    uint32_t token_id;        // 访问凭证（类似 rkey）
};

struct ubcore_ubva {
    union ubcore_eid eid;     // Endpoint ID
    uint64_t va;              // Virtual Address
};
```

### Token ID

```c
struct ubcore_token_id {
    uint32_t token_id;        // Token 值（类似 lkey/rkey）
};
```

### 工作请求

```c
struct ubcore_jfs_wr {
    enum ubcore_opcode opcode;  // WRITE/READ/SEND
    /* jetty / target jetty fields depend on the target ubcore headers */
    struct ubcore_rw_wr rw;     // READ/WRITE 操作
};

struct ubcore_sge {
    uint64_t addr;              // UBVA 地址
    uint32_t len;
    struct ubcore_target_seg *tseg;
};
```

## URMA 操作码

```c
UBCORE_OPC_WRITE      // 写（类似 RDMA WRITE）
UBCORE_OPC_READ       // 读（类似 RDMA READ）
UBCORE_OPC_SEND       // 发送（类似 RDMA SEND）
UBCORE_OPC_WRITE_IMM  // 带立即数写
UBCORE_OPC_CAS        // 比较并交换
```

## 配置为 NVMe-oF Target Namespace

```bash
# 创建 subsystem
cd /sys/kernel/config/nvmet/subsystems
mkdir mytarget
echo 1 > mytarget/attr_allow_any_port

# 创建 namespace
mkdir mytarget/namespaces/1
echo -n /dev/urma_blkdev > mytarget/namespaces/1/device_path
echo 1 > mytarget/namespaces/1/enable

# 创建 port
cd /sys/kernel/config/nvmet/ports
mkdir 1
echo rdma > 1/addr_trtype  # 或使用 URMA transport
echo <target_ip> > 1/addr_traddr
echo 4420 > 1/addr_trsvcid

# 关联 subsystem
ln -s /sys/kernel/config/nvmet/subsystems/mytarget 1/subsystems/mytarget
```

## Target 侧获取内存地址

当 host 侧写入数据时，target 侧可通过以下方式获取对应的内存地址：

### 从 sysfs 获取基地址

```bash
# 获取 ramdisk 基地址和 URMA 信息
ubva=$(cat /sys/block/urma_blkdev/ubva)
token_id=$(cat /sys/block/urma_blkdev/token_id)
size=$(cat /sys/block/urma_blkdev/ramdisk_size)
```

### LBA 到地址映射

```c
// 内核代码示例
uint64_t lba = nvme_req->slba;
uint32_t block_count = nvme_req->nlb + 1;

// 计算内存地址
void *mem_addr = ramdisk_addr + lba * logical_block_size;
size_t length = block_count * logical_block_size;

// 计算 UBVA 地址（用于 URMA 远程访问）
uint64_t ubva = seg_ubva_base + lba * logical_block_size;
uint32_t remote_token = token_id;  // 远程访问凭证
```

### URMA 远程访问

远程节点访问 target ramdisk：

```c
// 远程节点代码
struct ubcore_sge remote_sge;
remote_sge.addr = ubva + offset;  // 目标 UBVA 地址
remote_sge.len = length;
remote_sge.tseg = imported_seg;   // import 的 target segment

struct ubcore_jfs_wr wr;
wr.opcode = UBCORE_OPC_READ;      // 或 WRITE
wr.rw.dst.sge = &remote_sge;
wr.rw.dst.num_sge = 1;

ubcore_post_jetty_send_wr(jetty, &wr, &bad_wr);
ubcore_poll_jfc(jfc, cr_cnt, &cr);
```

## NVMe-oF Target 调用接口

本模块导出一个异步接口，供修改后的 NVMe-oF target 在解析自定义命令后调用：

```c
#include <linux/urma_blkdev.h>

int urma_blkdev_submit_nvmet_io(struct block_device *bdev,
                                const struct urma_blkdev_peer *peer,
                                const struct urma_blkdev_io *io,
                                urma_blkdev_done_fn done,
                                void *priv);
```

`bdev` 建议传 `nvmet` namespace 对应的 `struct block_device *`。当前模块只有一个全局设备，`bdev == NULL` 时也会尝试使用全局设备，但正式集成时不要依赖这个调试路径。

NVMe-oF target 侧示例：

```c
static void nvmet_urma_done(void *priv, int status)
{
    struct nvmet_req *req = priv;

    nvmet_req_complete(req, status ? NVME_SC_INTERNAL : NVME_SC_SUCCESS);
}

ret = urma_blkdev_submit_nvmet_io(req->ns->bdev, &peer, &io,
                                  nvmet_urma_done, req);
if (ret)
    nvmet_req_complete(req, NVME_SC_INTERNAL);
```

方向语义：

| NVMe 命令 | 本模块方向 | URMA 操作 |
|-----------|------------|-----------|
| host read | `URMA_BLKDEV_IO_READ` | WRITE: ramdisk -> HBM |
| host write | `URMA_BLKDEV_IO_WRITE` | READ: HBM -> ramdisk |

## 队列管理

`urma_blkdev_submit_nvmet_io()` 返回 `0` 只表示请求已经进入本模块队列，不表示 IO 完成。真正完成必须等待 `done(priv, status)` 回调。

模块内部维护：

- `pending_list`：已接受但尚未 post 到 jetty 的请求
- `inflight_reqs`：已经分配 `wr_id` 并 post/等待完成的请求
- `submit_work`：从 pending 队列取请求并尝试 post URMA WR
- `queue_depth`：限制 pending + inflight 数量，避免调试时无限堆积请求

模块参数：

```bash
sudo insmod urma_blkdev.ko device_size_mb=1024 ub_dev_name=ub0 queue_depth=1024
```

查看队列状态：

```bash
cat /sys/block/urma_blkdev/direct_io_queue
```

示例输出：

```text
queue_depth=1024
pending=0
inflight=0
stopping=0
next_wr_id=0
```

## 当前 fail-safe 行为

当前代码已经实现了 NVMe-oF 可调用接口、请求入队、`wr_id` 分配、inflight 跟踪、异常回调和卸载清理。

真实 `ubcore_post_jetty_send_wr()` 的 WR 字段还需要根据目标环境的 URMA 内核头文件确认。因此当前 `urma_blkdev_post_jetty_wr()` 会安全返回 `-EOPNOTSUPP`，并打印完整调试信息：

- `wr_id`
- 方向
- `lba`
- `block_count`
- 本地 `local_ubva`
- HBM 地址
- HBM token
- peer jetty 指针

这样可以先联调 NVMe-oF 到本模块的调用链、参数传递和 completion 回调，不会因为猜测 ubcore WR 结构体字段导致内核崩溃。确认目标环境 jetty WR API 后，只需要替换 `urma_blkdev_post_jetty_wr()`。

## 卸载模块

```bash
sudo rmmod urma_blkdev
```

## 注意事项

1. **URMA 设备要求**：模块需要 URMA 设备支持（华为 UB 硬件）
2. **内存类型**：使用 `vmalloc_user` 分配内存，支持大容量 ramdisk
3. **EID 索引**：用于多 EID 场景，默认为 0
4. **NVMe-oF 集成**：需要修改 NVMe-oF target 代码来利用 URMA segment 信息

## 与 kernel/ 目录的区别

| 项目 | kernel/ (RDMA) | kernel-urma/ (URMA) |
|------|----------------|---------------------|
| 传输协议 | RDMA (ib_verbs) | URMA (ubcore) |
| 内存注册 | ib_reg_user_mr | ubcore_register_seg |
| 地址类型 | IOVA | UBVA |
| 访问凭证 | lkey/rkey | token_id |
| 头文件 | rdma/ib_verbs.h | ub/urma/ubcore_*.h |
