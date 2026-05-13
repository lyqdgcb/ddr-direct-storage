# RDMA Ramdisk Block Device Driver

创建一个 RDMA-enabled ramdisk 块设备，可作为 NVMe-oF target namespace 后端使用。

## 编译

```bash
cd kernel
make
```

## 加载模块

```bash
# 加载模块，指定大小和 RDMA 设备
sudo insmod rdma_blkdev.ko device_size_mb=1024 ib_dev_name=mlx5_0

# 或不指定 RDMA 设备（纯内存模式）
sudo insmod rdma_blkdev.ko device_size_mb=256
```

## 模块参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `device_size_mb` | 256 | 设备大小（MB） |
| `logical_block_size` | 512 | 逻辑块大小 |
| `ib_dev_name` | NULL | RDMA 设备名（如 mlx5_0） |

## 查看设备信息

```bash
# 查看块设备
lsblk

# 查看 RDMA MR 信息
cat /sys/block/rdma_blkdev/mr_info

# 输出示例：
# lkey=0x12345678
# rkey=0x12345679
# iova=0x1000000000
# size=1073741824
# addr=0xffff888012340000

# 查看单个属性
cat /sys/block/rdma_blkdev/lkey
cat /sys/block/rdma_blkdev/rkey
cat /sys/block/rdma_blkdev/iova
```

## 配置为 NVMe-oF Target Namespace

### 方法 1: 使用 nvmetcli

```bash
nvmetcli
# 在交互界面中配置 namespace 使用 /dev/rdma_blkdev
```

### 方法 2: 手动配置 configfs

```bash
# 创建 subsystem
cd /sys/kernel/config/nvmet/subsystems
mkdir mytarget
echo 1 > mytarget/attr_allow_any_port

# 创建 namespace
mkdir mytarget/namespaces/1
echo -n /dev/rdma_blkdev > mytarget/namespaces/1/device_path
echo 1 > mytarget/namespaces/1/enable

# 创建 port
cd /sys/kernel/config/nvmet/ports
mkdir 1
echo rdma > 1/addr_trtype
echo <target_ip> > 1/addr_traddr
echo 4420 > 1/addr_trsvcid

# 关联 subsystem
ln -s /sys/kernel/config/nvmet/subsystems/mytarget 1/subsystems/mytarget
```

## Host 侧连接

```bash
# 连接到 target
nvme connect -t rdma -a <target_ip> -s 4420 -n mytarget

# 查看设备
nvme list

# 测试读写
dd if=/dev/zero of=/dev/nvme0n1 bs=1M count=100
dd if=/dev/nvme0n1 of=/dev/null bs=1M count=100
```

## 卸载模块

```bash
sudo rmmod rdma_blkdev
```

## Target 侧获取写入内存地址

当 host 侧写入数据时，target 侧可通过以下方式获取对应的内存地址：

### 从 sysfs 获取基地址

```bash
# 获取 ramdisk 基地址
base_addr=$(cat /sys/block/rdma_blkdev/ramdisk_addr)
size=$(cat /sys/block/rdma_blkdev/ramdisk_size)
```

### LBA 到地址映射

```c
// 内核代码示例
uint64_t lba = nvme_req->slba;
uint32_t block_count = nvme_req->nlb + 1;

// 计算内存地址
void *mem_addr = ramdisk_addr + lba * logical_block_size;
size_t length = block_count * logical_block_size;

// 计算 IOVA 地址（用于 RDMA）
uint64_t iova = ramdisk_iova + lba * logical_block_size;
```

### 在 NVMe-oF target 中集成

修改 NVMe-oF target 代码，在处理 IO 命令时：

```c
// 获取 rdma_blkdev 设备
struct rdma_blkdev_device *dev = bdev->bd_disk->private_data;

// Host NVMe WRITE -> Target RDMA READ
if (direction == NVME_CMD_WRITE) {
    // 从 host buffer RDMA READ 到 target ramdisk
    uint64_t local_iova = dev->ramdisk_iova + (lba * block_size);
    uint64_t remote_addr = host_buffer_addr;  // 来自 NVMe 命令
    uint32_t rkey = host_rkey;                // 来自 NVMe 命令
    
    // 执行 RDMA READ
    ib_post_rdma_read(qp, local_iova, dev->lkey, 
                      remote_addr, rkey, length);
}

// Host NVMe READ -> Target RDMA WRITE
if (direction == NVME_CMD_READ) {
    // 从 target ramdisk RDMA WRITE 到 host buffer
    uint64_t local_iova = dev->ramdisk_iova + (lba * block_size);
    uint64_t remote_addr = host_buffer_addr;
    uint32_t rkey = host_rkey;
    
    // 执行 RDMA WRITE
    ib_post_rdma_write(qp, local_iova, dev->lkey,
                       remote_addr, rkey, length);
}
```

## 注意事项

1. **RDMA 设备要求**：模块需要 RDMA 设备（如 Mellanox mlx5）来注册 MR
2. **内存类型**：使用 `vmalloc_user` 分配内存，支持大容量 ramdisk
3. **DMA 映射**：内存通过 `ib_dma_map_single` 映射为 IOVA
4. **NVMe-oF 集成**：需要修改 NVMe-oF target 代码来利用 RDMA MR 信息