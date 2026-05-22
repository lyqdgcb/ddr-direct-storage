# URMA Userspace Notes

来源：`external/umdk`，下载自 `https://gitcode.com/openeuler/umdk.git`，当前分析提交为 `fb8ec246`。重点文件：

- `external/umdk/src/urma/examples/urma_sample.c`
- `external/umdk/src/urma/lib/urma/core/include/urma_api.h`
- `external/umdk/src/urma/lib/urma/core/include/urma_types.h`
- `external/umdk/src/urma/lib/urma/core/include/urma_opcode.h`

## 关键对象

URMA 的用户态接口和 RDMA verbs 很像，但命名不同：

- `urma_context_t`: 绑定某个 URMA 设备和 EID 的上下文。
- `urma_jfce_t`: JFC event channel，用于事件模式等待完成。
- `urma_jfc_t`: Jetty for Completion，完成队列。
- `urma_jfs_t`: Jetty for Send，发送队列。示例没有单独创建 JFS，而是通过 `urma_create_jetty()` 内含 JFS。
- `urma_jfr_t`: Jetty for Receive，接收队列。
- `urma_jetty_t`: JFS + JFR 的组合对象，可直接 `post_jetty_send_wr` / `post_jetty_recv_wr`。
- `urma_target_seg_t`: 本地注册或远端导入后的内存段句柄。
- `urma_target_jetty_t`: 远端 JFR/jetty 导入后的目标句柄，发送 WR 使用它作为目的端。

## 初始化和资源创建流程

`urma_sample.c` 的 `main()` 先调用 `init_urma_lib()`：

1. 填 `urma_init_attr_t`，示例设置 `.uasid = 0`，让系统分配 UASID。
2. 调 `urma_init(&init_attr)` 初始化 URMA 运行环境。
3. 退出前调 `urma_uninit()`。

每个 client/server 线程在 `init_context()` 里创建一套本地资源：

1. `urma_get_device_by_name(args->dev_name)` 获取设备。
2. `urma_query_device(dev, &dev_attr)` 查询能力，示例用 `dev_attr.dev_cap.max_jfc_depth` 配 JFC depth。
3. `urma_get_eid_list(dev, &eid_cnt)` 获取 EID 列表，示例取第一个 `eid_index`，随后 `urma_free_eid_list()`。
4. `urma_create_context(dev, eid_index)` 创建上下文。
5. `urma_create_jfce(ctx)` 创建事件通道。
6. `urma_create_jfc(ctx, &jfc_cfg)` 创建完成队列，`jfc_cfg.jfce = jfce`。
7. 如果启用事件模式，先 `urma_rearm_jfc(jfc, false)`。
8. `urma_create_jfr(ctx, &jfr_cfg)` 创建接收队列，关键字段包括：
   - `depth`
   - `trans_mode`
   - `jfc`
   - `token_value`
   - `max_sge`
9. 构造 `urma_jfs_cfg_t`，再构造 `urma_jetty_cfg_t`，示例设置 `flag.bs.share_jfr = 1`，并把 `shared.jfr = ctx->jfr`。
10. `urma_create_jetty(ctx, &jetty_cfg)` 创建本地 jetty。

传输模式由参数映射：

- `0 -> URMA_TM_RM`
- `1 -> URMA_TM_RC`
- `2 -> URMA_TM_UM`
- `3 -> URMA_TM_RC`，但设置 `order_type/share_tp`，示例称为 RS 风格组合。

TP 类型由参数映射：

- `0 -> URMA_RTP`
- `1 -> URMA_CTP`
- `2 -> URMA_UTP`

## 内存注册和远端段导入

示例分配 1 GiB 页对齐内存：

1. `memalign(PAGE_SIZE, MEM_SIZE)`
2. 清零内存。
3. 构造 `urma_reg_seg_flag_t`：
   - `token_policy = URMA_TOKEN_NONE`
   - `cacheable = URMA_NON_CACHEABLE`
   - `access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC`
4. 构造 `urma_seg_cfg_t`：
   - `va = (uint64_t)ctx->va`
   - `len = MEM_SIZE`
   - `token_value = ctx->token`
   - `flag = flag`
   - `iova = 0`
5. `urma_register_seg(ctx, &seg_cfg)` 得到 `ctx->local_tseg`。

对端无法凭普通虚拟地址直接访问本地内存；双方先用 TCP socket 交换 `seg_jetty_info_t`，里面包含：

- `eid`
- `uasid`
- `seg_va`
- `seg_len`
- `seg_flag`
- `seg_token_id`
- `jetty_id`

收到对端信息后填 `ctx->remote_seg` 和 `ctx->remote_jetty_id`。client 随后导入远端内存段：

1. 构造 `urma_import_seg_flag_t`：
   - `cacheable = URMA_NON_CACHEABLE`
   - `access = READ | WRITE | ATOMIC`
   - `mapping = URMA_SEG_NOMAP`
2. `urma_import_seg(ctx, &ctx->remote_seg, &ctx->token, 0, flag)` 得到 `ctx->c.import_tseg`。

这里的 `URMA_SEG_NOMAP` 表示本进程不把远端段映射成可直接 load/store 的本地地址；示例通过 WR 中的远端 UBVA 地址和 imported tseg 做 READ/WRITE。

## Jetty 连接和通信

示例用普通 TCP 作为 out-of-band 控制面，只负责交换 URMA 地址和 jetty 元数据；真正数据面走 URMA。

远端 jetty 导入流程在 `sample_import_jetty()`：

1. 用 `ctx->remote_jetty_id` 构造 `urma_rjetty_t`。
2. 设置 `trans_mode`、`type = URMA_JETTY`、`tp_type`。
3. `urma_import_jetty(ctx, &remote_jetty, &ctx->token)` 得到 `urma_target_jetty_t`。
4. 若是 RC/RS 模式，调用 `urma_bind_jetty(ctx->jetty, t_jetty)` 建立本地 jetty 到远端 jetty 的连接。

### RDMA WRITE/READ

client 的 `client_write_read()` 展示了一次 WRITE + READ：

1. 本地 buffer 写入消息。
2. 构造本地 `urma_sge_t src_sge`：
   - `addr = local va`
   - `len = MSG_SIZE`
   - `tseg = local_tseg`
3. 构造远端 `urma_sge_t dst_sge`：
   - `addr = remote_seg.ubva.va`
   - `len = MSG_SIZE`
   - `tseg = import_tseg`
4. 构造 `urma_rw_wr_t { .src = src_sg, .dst = dst_sg }`。
5. 构造 `urma_jfs_wr_t`：
   - `opcode = URMA_OPC_WRITE`
   - `flag.bs.complete_enable = 1`
   - `tjetty = imported remote jetty`
   - `user_ctx = request id`
   - `rw = rw`
6. `urma_post_jetty_send_wr(local_jetty, &wr, &bad_wr)`。
7. 轮询完成，确认 `cr.status == URMA_CR_SUCCESS` 且 `cr.user_ctx == request id`。
8. READ 时交换 `rw.src` / `rw.dst`，把 `opcode` 改为 `URMA_OPC_READ`，再 post 和 poll。

### SEND/RECV

SEND/RECV 需要接收端先投递 receive buffer：

server 的 `server_jetty_thread_main()`：

1. 等待至少一个 client 完成 TCP 元数据交换和 jetty import。
2. 从本地注册内存中切出若干 `MSG_SIZE` buffer。
3. 对每个 buffer 构造 `urma_jfr_wr_t`，设置 `src = recv sg`、`user_ctx = offset`。
4. 调 `urma_post_jetty_recv_wr(ctx->jetty, &wr, &bad_wr)`。
5. 收到 `URMA_CR_OPC_SEND` 完成后，用 `cr.user_ctx` 找到 buffer offset，处理消息。
6. 复用同一个 buffer，再次 `urma_post_jetty_recv_wr()`。

client 的 `client_send()`：

1. 先在 client 本地 jetty 上投递一个 receive buffer，用来接收 server response。
2. 准备发送内容。
3. 构造 `urma_send_wr_t` 和 `urma_jfs_wr_t`：
   - `opcode = URMA_OPC_SEND`
   - `flag.bs.complete_enable = 1`
   - `tjetty = imported remote jetty`
   - `user_ctx = request id`
4. 调 `urma_post_jetty_send_wr()`。
5. poll 两个完成：一个是本地 send 完成，一个是 server response 的 recv 完成。示例用 `cr.flag.bs.s_r` 区分 send/recv，`0` 表示 send，`1` 表示 recv。

server 收到 client SEND 后，`server_reply_to_client()` 会根据 `cr.remote_id` 找到对应 `t_jetty`，再用 `URMA_OPC_SEND` 回包。

## 完成处理

示例封装在 `poll_jfc_wait()`：

轮询模式：

1. 循环调用 `urma_poll_jfc(jfc, 1, &cr)`。
2. 返回值 `< 0` 是错误，`0` 表示暂无完成，`> 0` 表示拿到 CR。
3. 要检查 `cr.status == URMA_CR_SUCCESS`。
4. 示例最多轮询 `MAX_POLL_JFC_CNT` 次，每次 sleep 100 ms。

事件模式：

1. `urma_wait_jfc(jfce, 1, TIMEOUT, &ev_jfc)` 等待完成事件。
2. 确认返回的 JFC 是目标 JFC。
3. `urma_poll_jfc(jfc, 1, &cr)` 拉取 CR。
4. `urma_ack_jfc(&ev_jfc, &ack_cnt, 1)` 确认事件。
5. `urma_rearm_jfc(jfc, false)` 重新 arm。

`urma_cr_t` 的关键字段：

- `status`: 必须检查是否为 `URMA_CR_SUCCESS`。
- `user_ctx`: post WR 时带入的请求标识，可用于匹配 IO。
- `opcode`: recv 完成上用于区分 SEND、SEND_WITH_IMM、WRITE_WITH_IMM 等。
- `flag.bs.s_r`: `0` 表示 send-side completion，`1` 表示 recv-side completion。
- `remote_id`: recv 完成上可用于识别消息来自哪个远端 jetty。
- `imm_data`: immediate data。
- `completion_len`: 完成的数据长度。

## 资源释放顺序

client 释放：

1. `urma_unimport_jetty(ctx->c.t_jetty)`
2. `urma_unimport_seg(ctx->c.import_tseg)`
3. 进入通用 `uninit_context()`

server 释放：

1. 停止 server 线程。
2. 关闭 client socket 和 listen socket。
3. server socket 线程里对每个 imported client jetty 调 `urma_unimport_jetty()`。
4. 进入通用 `uninit_context()`

通用 `uninit_context()` 顺序：

1. `urma_unregister_seg(local_tseg)`
2. `urma_delete_jetty(jetty)`
3. `urma_delete_jfr(jfr)`
4. `urma_delete_jfc(jfc)`
5. `urma_delete_jfce(jfce)`
6. `free()` 或 UB 设备下 `munmap()` 本地 buffer
7. `urma_delete_context(urma_ctx)`
8. `free(ctx)`

最后回到 `main()` 后 `urma_uninit()`。

## 对 userspace NBD/ramdisk 的启发

把当前 userspace ramdisk 内存接入 URMA 时，最小数据面形态可以参考示例：

1. ramdisk 后端分配一整块页对齐内存。
2. URMA 初始化后，把整块 ramdisk memory 用 `urma_register_seg()` 注册成一个 `local_tseg`。
3. 控制面向对端公布 `eid + uasid + seg.ubva.va + len + token_id + jetty_id`。
4. 对端导入 segment 和 jetty 后，按 LBA 计算远端地址：
   - `remote_addr = remote_seg.ubva.va + lba * block_size`
5. NVMe/NBD 读写方向映射到 URMA 操作时要明确“谁发起 RDMA”：
   - 若本进程主动把本地 ramdisk 数据推到对端，用 `URMA_OPC_WRITE`。
   - 若本进程主动从对端拉数据到本地 ramdisk，用 `URMA_OPC_READ`。
6. 每个 IO 的 request id 放进 `urma_jfs_wr_t.user_ctx`，完成时用 `cr.user_ctx` 匹配上层请求。
7. SEND/RECV 更适合小控制消息或完成通知；大块数据更适合 READ/WRITE。
8. 对高并发 IO，需要预先设计 JFC depth、JFR depth、outstanding WR 数量和 receive buffer 池，示例只演示功能，不是性能型实现。

## 注意点

- `urma_init()` / `urma_uninit()` 是进程级生命周期；`urma_create_context()` / `urma_delete_context()` 是设备上下文生命周期。
- `urma_register_seg()` 暴露给远端访问的能力由 `access` 和 token 决定，不能把 token/segment 元数据当成普通日志随意泄露。
- `URMA_SEG_NOMAP` 导入远端段后仍可用于 RDMA READ/WRITE WR，但不代表可直接用 CPU 指针访问远端内存。
- 使用 SEND/RECV 时，接收端必须先 post recv，否则可能触发 RNR 重试或错误。
- `complete_enable = 1` 才会产生本地完成；如果关闭完成，上层就不能用 JFC 来回收该 WR。
- 示例错误路径比较演示化，例如 `init_context()` 内部失败时会调用 `urma_uninit()`，而正常路径由 `main()` 统一 uninit；集成到长期运行 daemon 时建议统一进程级初始化/反初始化职责。
