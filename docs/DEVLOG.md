# DevLog

用于沉淀会影响后续实现、并发模型或模块边界的架构问题与决策。每一条记录独立编号，后续新增内容应追加为新的 `ARC-XXX` 条目，而不是混入已有条目。

## 记录索引

- [ARC-001：TCP 接收路径的跨 lcore 所有权缺陷](#arc-001tcp-接收路径的跨-lcore-所有权缺陷) — 接收路径已实施；生命周期收敛待后续处理
- [ARC-002：Socket 单 owner、代际句柄与命令队列](#arc-002socket-单-owner代际句柄与命令队列) — 已实施；取代跨 lcore 裸指针与 `tcp_rx_events` 过渡模型
- [ARC-003：traffic-gen owner-local 队列与按需 TCP 缓冲](#arc-003traffic-gen-owner-local-队列与按需-tcp-缓冲) — 实施中；消除 per-socket ring 后继续收敛 TCB 内存

## 条目格式

每个架构记录应包含：

1. **状态**：已识别 / 设计中 / 实施中 / 已实施 / 已废弃。
2. **范围**：受影响的模块、lcore 或 API。
3. **问题与证据**：当前行为、触发条件和风险。
4. **架构原则与推荐方案**：明确长期应遵守的所有权和数据流。
5. **实施要点与遗留事项**：后续改动的边界、依赖和验证重点。

---

## ARC-001：TCP 接收路径的跨 lcore 所有权缺陷

- **状态**：接收路径已实施；socket 生命周期收敛待后续处理。
- **范围**：`pro-stack/tcp.c`、`socket.c`、worker lcore、app lcore、`recv_buf`、TCP 接收流控。
- **触发**：实现 `rcvbuf_used` / 接收窗口更新时发现 app 消费数据与 worker 重组数据共享 TCP 接收状态。
- **长期原则**：worker 是 TCP 状态唯一 owner；app 只消费已交付数据并向 worker 发送事件。

### 背景

当前协议栈运行模型中，worker lcore 负责收包、TCP 状态机、乱序重组和发包；app lcore 调用 `nrecv()` / `tcp_recv()` 读取应用数据。TCP 已有：

- `recv_buf`：向应用交付按序 `tcp_rx_blob` 的环；
- `ofo`：保存尚不能按序交付的乱序段；
- `recv_ack`：下一个期待的对端序号，也是本端累计 ACK；
- `rcvbuf_used`：接收缓存中尚未被应用读取的逻辑字节数。

在实现收端流控时，发现这些对象的读写所有权没有完全划分清楚。

### 现有职责与数据流

```text
NIC
  │
  ▼
worker lcore
  ├─ tcp_ingress()
  ├─ tcp_state_established()
  ├─ tcp_ofo_insert() / tcp_ofo_drain()
  ├─ recv_buf（生产 tcp_rx_blob）
  └─ 发送 ACK / window-update
                   │
                   ▼
              app lcore
              └─ tcp_recv()（消费 tcp_rx_blob）
```

`recv_buf` 当前按单生产者、单消费者模式创建：

```c
sk->recv_buf = rte_ring_create(recv_name, RING_SIZE, rte_socket_id(),
                               RING_F_SP_ENQ | RING_F_SC_DEQ);
```

设计意图应是 worker 为唯一生产者，app 为唯一消费者。

### 问题一：应用释放空间后，协议层不会立即继续重组

场景：

```text
recv_buf: [A, B]   # 已满
ofo:      [C]      # C 的序号紧随 B，但此前无法放入 recv_buf
```

1. app 调用 `tcp_recv()` 并读取 A，`recv_buf` 出现可用空间。
2. 为了继续按序交付，协议层需要把 C 从 `ofo` 移到 `recv_buf`。
3. 同时需要推进 `recv_ack`，并向对端发送更新后的接收窗口 ACK。
4. 但 `tcp_ofo_drain()` 当前仅从 worker 的收包路径调用。app 读取完成并不会通知 worker。

结果是 C 可能一直滞留在 `ofo`，直到对端又发送新报文或重传，worker 才会再次运行 `tcp_ofo_drain()`。这会产生不必要的延迟和重传，也使“应用已释放缓存空间”不能及时反映在通告窗口中。

### 问题二：app 不能直接调用 `tcp_ofo_drain()`

这不是语言层面的限制，而是当前并发模型不允许。

`tcp_ofo_drain()` 会同时修改：

- `ofo` 链表及其中节点；
- `ofo_count`；
- `recv_ack`；
- 收到 FIN 时的 TCP 状态；
- `recv_buf`（通过 `tcp_deliver_payload()` 生产新的应用数据）。

worker 也会在 `tcp_ofo_insert()`、`tcp_ofo_drain()` 与状态机中并发读写这些字段。现有代码没有覆盖这些 TCP 接收状态的一把锁。

若 app 和 worker 同时 drain / 插入，可能发生：

```text
worker：读取 ofo 头节点 C
app：   也读取 ofo 头节点 C
worker：从链表摘除并释放 C
app：   继续访问或释放 C
```

后果包括 use-after-free、双重释放、链表损坏，以及 `recv_ack` / TCP 状态不一致。

此外，`tcp_ofo_drain()` 会调用 `tcp_deliver_payload()`，后者使用 `rte_ring_sp_enqueue()` 向 `recv_buf` 写入。若 app 也调用 drain，worker 与 app 会成为并发生产者，违反 `RING_F_SP_ENQ` 的单生产者前提。

### 问题三：短读时将未读 blob 重新入队会破坏 TCP 字节流顺序

当前 `tcp_recv()` 对一个 blob 的短读会把该 blob 放回 `recv_buf` 尾部：

```c
if (b->off < b->len) {
        rte_ring_mp_enqueue(sk->recv_buf, b);
        return n;
}
```

若队列原本为：

```text
recv_buf: [A, B]
```

app 取出 A，只读一部分后重新入队，结果为：

```text
recv_buf: [B, A 的剩余部分]
```

下一次 `tcp_recv()` 会先返回 B 的字节，再返回 A 的剩余字节，违反 TCP 必须以严格字节序交付给应用的语义。

这段代码还使 app lcore 通过 `rte_ring_mp_enqueue()` 反向写入原本由 worker 使用 `rte_ring_sp_enqueue()` 生产的 ring；两种生产路径并发时不满足 ring 的生产者模型。

### 对收端流控的影响

`rcvbuf_used` 应表示应用尚未读取的全部逻辑字节：

```text
rcvbuf_used =
    recv_buf 中未读字节
  + ofo 中尚未按序交付的字节
```

本端通告窗口应为：

```text
rcv_wnd = rcvbuf_size - rcvbuf_used
```

因此应用消费字节不仅是应用层事件，也是 TCP 协议状态变化：它会扩大 `rcv_wnd`，可能允许 OFO drain，并需要产生 window-update ACK。该状态变化必须由 TCP 接收状态的唯一 owner 串行处理。

### 推荐架构：worker 独占 TCP 状态，app 发送消费事件

#### 所有权规则

worker lcore 独占修改以下 TCP 状态：

- `recv_ack`；
- `rcvbuf_used`；
- `ofo` / `ofo_count`；
- TCP 状态机；
- ACK 和 window-update ACK；
- 长期也应包含 `sndbuf`、`sent_seq`、`snd_una`、`snd_wnd`。

app lcore 仅：

- 从 `recv_buf` 消费已经按序交付的数据；
- 保留自己的短读剩余数据；
- 向 worker 报告“已消费 N 字节”；
- 等待 API 操作的完成结果。

#### app 到 worker 的事件队列

增加一个 app → worker 的 TCP 事件 ring。事件至少包含：

```c
enum tcp_event_type {
        TCP_EVENT_RX_CONSUMED,
        TCP_EVENT_SEND,
        TCP_EVENT_CLOSE,
};

struct tcp_event {
        enum tcp_event_type type;
        struct nsock *sk;
        uint32_t bytes;
};
```

如果允许多个 app lcore，事件 ring 应为多生产者、单消费者；worker 是唯一消费者。

当 app 实际读走 `n` 字节时，不直接修改 `rcvbuf_used`，而是投递：

```text
TCP_EVENT_RX_CONSUMED(sk, n)
```

worker 处理该事件时：

1. 从 `rcvbuf_used` 扣除已消费字节；
2. 调用 `tcp_ofo_drain(sk)`，将现在可交付的连续 OFO 数据放入 `recv_buf`；
3. 构造 ACK，携带新的 `recv_ack` 和 `rcv_wnd`；
4. 将 ACK 放入发送路径。

这保证 OFO、累计 ACK、窗口通告和 TCP 状态迁移始终在同一个 lcore 串行发生。

#### 短读的正确处理

不要把未读 blob 重新放回 `recv_buf`。在 `nsock` 或 app 私有上下文中保存：

```c
struct tcp_rx_blob *rx_current;
```

接收逻辑：

```text
rx_current 为空：从 recv_buf 取出下一个 blob
复制所需字节
blob 未耗尽：保留为 rx_current
blob 耗尽：释放并将 rx_current 置空
```

因此 A 未读完时，下一次仍先读 A，只有 A 完全读完后才取 B。

#### 事件去重

每次小粒度 `recv()` 都分配并投递事件成本较高。可在 `tcp_stream` 中使用：

```c
atomic_uint rx_consumed;
atomic_bool rx_event_pending;
```

app 原子累积已消费字节；只有从“未挂起”变为“已挂起”时才投递一次 socket。worker 用原子交换批量取走消费计数，再处理 drain 和 ACK。实现时必须在清除 `rx_event_pending` 后复查计数，避免 app 与 worker 交错时丢失通知。

### 生命周期要求

事件若携带 `struct nsock *`，socket 释放前必须保证没有待处理事件继续引用该指针。长期应将 `nclose()` 也变为 worker 事件，并由 worker 统一停止 timer、摘除活跃队列、清理接收/发送状态和最终释放 socket。

更严格的实现可让事件携带 `{fd, generation}`，worker 查表并验证 generation，防止 fd 复用导致旧事件操作新连接（ABA 问题）。

### 本次实施（2026-07-29）

- 新增全局 MPSC、worker 单消费者的 `tcp_rx_events` ring；每个 socket 通过 `rx_event_pending` 合并多次读取事件。
- app 仅原子累计 `rx_consumed` 并投递 socket；worker 调用 `tcp_process_app_events()` 统一扣减 `rcvbuf_used`、drain OFO、发送 window-update ACK。
- `tcp_recv()` 使用 app 私有的 `rx_current` 保存短读剩余 blob，不再回写 `recv_buf`，保持 TCP 字节序和 worker 单生产者模型。
- worker 在每轮收包前后处理 app 事件，避免应用释放缓存空间后等待下一次网络报文或重传。
- 已执行 `make`，`pro-stack` 构建通过。

#### 修改思路与实现步骤

本次改动的目标不是让 app lcore “也能操作 TCP”，而是让 app 将已经发生的消费结果可靠地交给 TCP 的唯一 owner。数据路径被拆成两条单向路径：

```text
worker  -- 已按序的 tcp_rx_blob -->  recv_buf  --> app
app     -- 已消费字节数/通知      -->  tcp_rx_events --> worker
```

这保留了 `recv_buf` 的 SPSC 模型：worker 始终是唯一生产者，app 始终是唯一消费者。

**步骤 1：为消费通知建立独立的 MPSC ring**

- 在 `struct inout_ring` 中增加 `tcp_rx_events`。
- ring 中保存 `struct nsock *`，而非为每次 `recv()` 分配一个单独事件对象。
- 创建 ring 时使用 `RING_F_SC_DEQ`：允许一个或多个 app lcore 作为生产者，而 worker 是唯一消费者。
- `TCP_EVENT_RING_SIZE` 设置为 2048，大于 `NSOCK_FD_MAX`；每个可见 socket 同时至多挂起一个通知，因此正常情况下不会因 app 读取而填满。

这里没有复用 `recv_buf` 或 `send_buf`，因为它们分别承担 payload 交付和 TCP 控制段发送；把消费通知混入这些队列会混淆对象类型和队列所有权。

**步骤 2：将 app 的动作限制为“累计 + 唤醒”**

`tcp_recv()` 成功复制 `n` 字节后：

1. 使用 `atomic_fetch_add(rx_consumed, n)` 累计已经真正交给应用的字节数；
2. 使用 `rx_event_pending` 去重；
3. 仅在 pending 从 `false` 变为 `true` 时，将 socket 放入 `tcp_rx_events`。

这意味着连续的小读不会产生一条事件一个 ring entry。worker 最终看到的是一个合并后的消费字节总数。

app 不再直接修改 `rcvbuf_used`、`recv_ack`、OFO 链表或 TCP 状态。这些字段继续只由 worker 写入。

**步骤 3：worker 接收通知后串行恢复协议状态**

`tcp_process_app_events()` 在 `pkt_worker()` 的收包前后执行。对于每个通知：

1. `atomic_exchange(rx_consumed, 0)` 原子取走累计消费量；
2. worker 从 `rcvbuf_used` 扣除该数量，得到新的实际可用接收空间；
3. 执行 `tcp_ofo_drain()`：若原先因 `recv_buf` 满而卡在 OFO 的连续段现在可交付，则转入 `recv_buf` 并推进 `recv_ack`；
4. 构造纯 ACK。该 ACK 读取新的 `recv_ack` 和 `tcp_rcv_wnd()`，因而既确认新交付的数据，也向对端通告扩大后的接收窗口。

将事件处理放在收包前后有两个目的：

- worker 空闲但 app 正在读取时，下一轮循环即可处理窗口更新，不依赖新的网络包；
- app 在本轮处理网络包期间完成读取时，worker 在冲洗发送队列前仍能处理该通知。

**步骤 4：处理 `rx_event_pending` 清除时的竞态**

worker 处理一个 socket 后会清除 `rx_event_pending`。此时 app 可能正好新增一次读取。实现采用“清 pending 后检查计数”的循环：

```text
worker 取走 rx_consumed
worker 处理 OFO / ACK
worker 清除 pending
若 rx_consumed 仍非零：
  - 若 app 已重新置 pending 并入队，当前 worker 返回，后续事件处理；
  - 否则 worker 自己重新认领 pending，并在本地继续处理。
```

其不变量是：只要 `rx_consumed != 0`，要么 socket 已在事件 ring 中，要么当前 worker 正在处理它；不会因为 app 与 worker 的交错而永久丢失消费通知。

**步骤 5：用 app 私有 `rx_current` 修复短读**

此前短读会将 blob 放回 `recv_buf` 尾部。新逻辑如下：

```text
若 rx_current 为空：
  app 从 recv_buf 取一个 blob，保存到 rx_current

每次 tcp_recv：
  从 rx_current 拷贝最多请求长度的字节
  若未读完：保持 rx_current 不变
  若读完：释放 blob，并将 rx_current 置空
```

因此后续读取始终先消费同一个 blob 的剩余字节，只有耗尽后才从 `recv_buf` 取下一段数据；不会出现 `[A, B]` 短读 A 后变为 `[B, A 剩余]` 的乱序。

**明确未纳入本次范围的事项**

- `nsend()` 仍直接操作 `sndbuf`，尚未完全遵循 worker 独占发送状态的长期原则；
- `nclose()` / `nsock_free()` 尚未改为 worker 事件，事件携带裸 `struct nsock *` 的生命周期收敛仍是后续架构项；
- worker 仍遍历 `g_sock_list` 调用 `tx_flush`，尚未替换为仅处理活跃 socket 的 dirty 队列；
- 暂未引入窗口更新阈值或 delayed ACK；当前每次观察到应用消费都会排一个 window-update ACK，优先保证正确性与可观察性。

### 结论与后续实施要点

不要通过“把 `recv_buf` 改成 MP，然后让 app 直接调用 `tcp_ofo_drain()`”来修复问题。这会扩大并发写状态范围，并使 ACK、重组和 FIN 状态迁移难以保证顺序。

应采用 actor/owner 模型：worker 是 TCP 状态唯一 owner；app 仅消费数据并投递事件。该模型既修复 OFO 卡住和短读乱序，也为后续接收窗口更新、发送流控、dirty socket 队列和多核扩展提供一致的架构基础。

---

## ARC-002：Socket 单 owner、代际句柄与命令队列

- **状态**：已实施。
- **实施日期**：2026-07-31。
- **范围**：`pro-stack/socket_owner.h`、`socket_owner.c`、`socket.h`、`socket.c`、`tcp.c`、`tcp.h`、`udp.c`、`main.c`、`ring.c`、`ring.h`、`sock_ops.h`、`Makefile`。
- **触发**：fd、四元组和 listener 索引虽然由 `registry_lock` 保护，但 lookup 解锁后返回的 `struct nsock *` 仍可与 app `nclose()`、TCP timer 和 worker packet path 并发，形成 use-after-free；`tcp_rx_events` 过渡方案也在 app→worker ring 中携带裸指针，无法独立解决 socket 生命周期。
- **架构决策**：采用 actor/owner 模型。packet worker 是 `nsock`、TCP TCB、协议索引、TCP timer 和最终释放的唯一执行者；app lcore 只持有整数 fd，通过代际句柄和 MPSC command ring 请求 owner 执行 BSD API。
- **与 ARC-001 的关系**：ARC-001 记录的 `rx_consumed + tcp_rx_events` 是接收路径的过渡实现。本条目将 SEND、RECV、CONNECT、ACCEPT、CLOSE、timer 和 free 全部收敛到 owner，因此已经删除 `tcp_rx_events`、`rx_consumed` 和 `rx_event_pending`；应用不再直接消费 `recv_buf` 或修改任何 TCP 状态。

### 一、原问题及其边界

旧 socket API 的基本形式为：

```text
app lcore
  └─ nsock_from_fd(fd)
       ├─ registry_lock 加锁
       ├─ fd_table[fd] 取出 nsock *
       └─ registry_lock 解锁
  └─ sk->ops->send/recv/connect/close(sk)
```

`registry_lock` 只保护查表动作，并不 pin 返回对象。下列交错仍然成立：

```text
app A                           app B / timer / worker
-----                           ----------------------
sk = nsock_from_fd(fd)
registry_lock 已释放
                                nsock_free(sk)
sk->ops->send(sk, ...)
```

此时 A 使用的是已释放对象。类似风险还存在于：

- worker 无锁遍历 `g_sock_list` 与其他 lcore 删除/释放 socket；
- main lcore 的 `tcp_timer_cb()` 修改或释放 worker 正在处理的 TCB；
- app 直接修改 `sndbuf`、`rx_current`，worker 同时处理 ACK、重组或 RTO；
- `tcp_rx_events` ring 保存 `nsock *`，socket 释放后仍可能有待消费事件；
- fd 或 owner slot 被快速复用时，延迟到达的旧操作可能命中新对象，即 ABA。

单纯增加 `nsock` 引用计数只能延迟 free，不能自然解决 TCP 字段由多个 lcore 并发修改、timer 执行归属和阻塞 API 卡住 packet worker 等问题。因此最终选择严格 owner，而不是把 refcount 作为长期数据路径。

### 二、核心不变量

本次实施后的正确性依赖以下不变量：

1. **只有 owner packet worker 可以把 socket 身份解析成 `struct nsock *`。**
2. **app lcore 只能保存 fd 或 `struct nsock_handle`，不能保存/解引用 `nsock *`。**
3. **command ring 中只传递 handle 和 API 参数，不传递 `nsock *`。**
4. **packet ingress、socket command、TCP timer、状态迁移、协议索引修改和 `nsock_free()` 在同一 owner lcore 串行执行。**
5. **transport hook 不得等待网络进展；不能立即完成时返回 `EAGAIN` 或 `EINPROGRESS`。**
6. **阻塞的是 app command，不是 packet worker。**
7. **fd 撤销与 TCP TCB 回收是两个不同生命周期。**
8. **owner slot 在释放内存前先取消发布并推进 generation。**

任何后续功能若需要 app 直接调用 TCP 内部函数、从非 owner lcore 执行 `nsock_free()`，或将 `nsock *` 放入跨 lcore 队列，都违反本条目的架构约束。

### 三、线程模型

```text
main lcore
  ├─ NIC RX burst → ring->in
  ├─ ring->out → NIC TX burst
  └─ 管理 ARP sweep 等基础设施 timer

packet worker / socket owner
  ├─ drain socket command ring
  ├─ drain ring->in 并执行 UDP/TCP ingress
  ├─ TCP 状态机、ACK、窗口、OFO、sndbuf
  ├─ tx_flush
  ├─ rte_timer_manage()：TCP RTO / TIME_WAIT
  └─ nsock_free()

app lcore
  ├─ 持有整数 fd
  ├─ fd → generation handle
  ├─ 构造 sock_cmd 并提交
  └─ 等待 command completion
```

worker 每轮在收包前后各处理一次 command，降低无网络流量时的 API 延迟，并使本轮收包期间提交的命令能在 TX flush 前得到处理。

### 四、三层 socket 身份

#### 1. 应用 fd

fd 是应用可见、范围为 `[0, NSOCK_FD_MAX)` 的小整数。它可以在 `nclose()` 后立即复用，不代表 TCB 的存储地址，也不再写入 owner 对象作为生命周期依据。

#### 2. 代际句柄

fd 表保存：

```c
struct nsock_handle {
        uint32_t id;
        uint32_t generation;
        uint16_t owner_lcore;
        uint8_t protocol;
};
```

字段含义：

- `id`：owner `slots[]` 的下标；
- `generation`：该 slot 当前对象的代数；
- `owner_lcore`：负责解析和执行该命令的 worker；
- `protocol`：TCP/UDP 类型校验。

#### 3. owner 私有指针

真正的对象指针仅存在于：

```text
g_owner.slots[handle.id] → struct nsock *
```

owner 解析 handle 时同时验证：

```text
owner 已初始化
id 在范围内
handle.owner_lcore == g_owner.lcore_id
slots[id] 非空
sk->generation == handle.generation
sk->protocol == handle.protocol
```

任何条件不满足都以 `EBADF` 完成命令。

### 五、generation 与 ABA 防护

初次采用 slot 时，generation 从 1 开始，0 保留为无效值。对象退休时按以下顺序执行：

```text
1. 完成/取消该 socket 上所有 waiter
2. slots[id] = NULL
3. generations[id]++
4. 从索引和活跃链表删除
5. 停 timer、释放 ring/缓冲区和 nsock
```

示例：

```text
旧对象 A：fd=3 → {id=12, generation=8}
nclose(A)
退休 A：slots[12]=NULL，generations[12]=9
新对象 B 复用 slot：{id=12, generation=9}
延迟的 A 命令：{id=12, generation=8} → 校验失败
```

即使 slot 下标和后续内存地址都被复用，旧命令也不能操作新对象。

### 六、fd 表与 close 线性化

`fd_table` 的 value 从 `nsock *` 改为 `nsock_handle`。它提供三个内部操作：

- `fd_publish(handle)`：为 CREATE/ACCEPT 返回的 handle 分配 fd；
- `fd_resolve(fd, &handle)`：普通 API 在 `registry_lock` 下复制 handle；
- `fd_take(fd, &handle)`：原子复制 handle 并清空 fd 项。

`fd_take()` 是 close 的线性化点。一旦成功：

- 后续 API 对旧 fd 返回 `EBADF`；
- 第二个并发 `nclose()` 返回 `EBADF`；
- fd 可以映射到另一个 generation handle；
- 旧 TCP TCB 仍可由 owner 继续执行 FIN、LAST_ACK、TIME_WAIT 和 2MSL。

并发操作已经复制旧 handle 也不会形成 UAF：它只携带值类型身份，最终由 owner 校验 generation 和 `app_closed`。

### 七、command ring 与 completion

每次 BSD API 调用在 app 栈上构造：

```c
struct sock_cmd {
        enum sock_cmd_type type;
        struct nsock_handle handle;
        union { /* CREATE/address/I/O/LISTEN 参数 */ } args;
        struct nsock_handle result_handle;
        ssize_t result;
        int error;
        pthread_mutex_t done_mutex;
        pthread_cond_t done_cond;
        bool done;
        struct sock_cmd *next;
};
```

当前支持：

```text
CREATE
BIND
CONNECT
LISTEN
ACCEPT
SEND
RECV
SENDTO
RECVFROM
CLOSE
```

command ring 是多 app producer、单 owner consumer，因此创建时只指定 `RING_F_SC_DEQ`。

`socket_owner_call()` 的同步流程：

```text
app 初始化 command completion
  ↓
MPSC enqueue
  ↓
app 在 cmd->done_cond 等待
  ↓
owner dequeue + owner_lookup(handle)
  ↓
立即执行，或把 cmd 挂入 owner waiter
  ↓
socket_owner_complete(result, error)
  ↓
app 醒来并返回 cmd.result / errno
```

command ring 满时不再返回 `ENOBUFS`，而是用 `rte_pause()` 对 producer 施加背压。该选择对 CLOSE 尤其重要：`nclose()` 已先撤销 fd，若随后丢弃 CLOSE command，将留下应用无法再引用的永久 TCB。

command 当前保存在调用线程栈上，因此调用者必须一直等到 owner completion；当前模型不支持在命令挂起期间直接取消线程、超时返回或销毁 coroutine。

### 八、owner waiter 与非阻塞 transport hook

如果 owner 在 transport hook 中等待 ACK、payload 或握手，worker 将无法继续处理使条件成立的网络包。因此协议 hook 改为单次非阻塞探测：

```text
能立即完成       → 返回结果
暂时不能完成     → -1 / EAGAIN
异步握手已启动   → -1 / EINPROGRESS
```

owner 根据 API flags 决定：

- `MSG_DONTWAIT`：把 `EAGAIN` 直接返回应用；
- 阻塞 API：把 command 放入 socket 的 owner-only FIFO；
- 状态进展后调用 wake helper 重试。

`nsock` 中的 waiter：

```text
recv_wait_head/tail    TCP RECV、UDP RECVFROM
send_wait_head/tail    TCP SEND 背压
connect_waiter         TCP CONNECT 单飞
accept_wait_head/tail  listener ACCEPT
```

这些链表只由 owner 操作，不需要跨 lcore 锁。

#### SEND

`tcp_send()` 不再 `pthread_cond_wait()`。它检查 ESTABLISHED、`sndbuf` 高水位和对端窗口：

```text
无空间          → EAGAIN
有部分空间      → short write
有足够空间      → 复制进入 sndbuf
非 ESTABLISHED  → EPIPE
```

ACK 释放 sndbuf 或对端窗口更新后，TCP 调用 `socket_owner_wake_send()`，按 FIFO 重试 waiter。

#### RECV / RECVFROM

owner 先把 command 放入 recv waiter，再立即调用 wake helper 探测。TCP/UDP 接收队列为空时返回 `EAGAIN`；ingress 交付数据后调用 `socket_owner_wake_recv()`。

TCP 短读仍通过 `sk->u.tcp.rx_current` 保留未读 blob，但该状态已从“app 私有”变为“owner 私有”。应用不再直接 dequeue `recv_buf`。读取后 owner 立即：

1. 扣减 `rcvbuf_used`；
2. 调用 `tcp_ofo_drain()`；
3. 生成携带新窗口的 ACK。

因此不再需要 ARC-001 的 `rx_consumed` 原子计数和 `tcp_rx_events` ring。

当状态为 CLOSE_WAIT/CLOSED 且已无排队 payload 时，TCP RECV 返回 0，提供标准 EOF 语义。

`socket_owner_wake_recv()` 在调用 transport hook 前先把 command 从队列摘除；原因是 `tcp_recv()` 可能 drain OFO，而 OFO delivery 又会递归触发 wake。若当前 command 仍在队列，递归 wake 会执行同一 command 两次。探测返回阻塞型 `EAGAIN` 时，再把 command 放回队首。

#### CONNECT

`tcp_connect()` 只执行：

```text
隐式 bind / 临时端口
注册四元组
构造 SYN
切 SYN_SENT
arm SYN timer
返回 EINPROGRESS
```

owner 保存 `connect_waiter` 后立刻回到 packet loop。后续结果：

- 合法 SYN+ACK：切 ESTABLISHED，完成 command；
- SYN 重试耗尽：`ETIMEDOUT`；
- RST：`ECONNRESET`；
- 并发 CLOSE：`ECANCELED`。

#### ACCEPT

listener 同时维护：

- TCP `accept_queue`：已完成握手但尚未 accept 的 child；
- `accept_wait`：正在等待 child 的应用 command。

最终 ACK 将 child 放入 `accept_queue` 后调用 `socket_owner_wake_accept()`。owner 取出 child，设置 `app_visible=true`，把 child handle 写入 command；app 醒来后再调用 `fd_publish()` 分配 fd。

因此 SYN_RECV 半连接会占 owner slot，但不会占应用 fd。若 fd 表已满，app 用返回的 handle 再提交 CLOSE，避免孤儿 child。

### 九、`app_visible` 与 `app_closed`

`app_visible` 表示对象是否已经通过 CREATE 或 ACCEPT 暴露给应用：

```text
SYN_RECV child               false
已完成握手、仍在 accept_queue false
ACCEPT 返回 handle            true
```

它主要用于 listener teardown：关闭 listener 时释放未 accept child，但已 accept 的连接必须存活，只清除其 `listener` 裸指针。

`app_closed` 表示 fd 已被撤销，但协议 teardown 可能尚未结束：

```text
nclose()
  ├─ fd_take：应用身份立即失效
  ├─ owner 设置 app_closed=true
  └─ TCP 继续 FIN_WAIT/TIME_WAIT
```

owner 对 `app_closed` socket 的后续非 CLOSE command 返回 `EBADF`。

### 十、close 和协议终态回收

owner 处理 CLOSE 时：

```text
设置 app_closed
取消 recv/send/connect/accept waiter
调用 protocol close hook
完成 CLOSE command
```

UDP 没有 wire teardown，因此 drain 队列并立即 `nsock_free()`。

TCP 根据状态执行：

- CLOSED：立即 drain/free；
- LISTEN：取消 accept，释放未 accept/半开 child，保留已 accept child，释放 listener；
- SYN_SENT：停止 timer，以 `ECANCELED` 完成 connect，立即 free；
- ESTABLISHED：排 FIN，进入 FIN_WAIT_1，CLOSE command 立即返回；
- CLOSE_WAIT：排 FIN，进入 LAST_ACK，CLOSE command 立即返回；
- FIN_WAIT_* / CLOSING / LAST_ACK / TIME_WAIT：teardown 已在运行，仅返回。

应用 `nclose()` 不再一直等待到 2MSL。fd 先失效，TCB 在 owner 中继续存在；最后由状态处理器或 TIME_WAIT timer 释放。

如果 FIN 分配/入队失败，fd 已经不可恢复。当前策略是本地转 CLOSED 并释放对象，返回 `ENOBUFS`；未来可增加 abortive RST。

### 十一、TCP timer 所有权

旧实现将 TCP timer 目标设为 main lcore，导致 main timer callback 与 packet worker 并发修改同一 TCB。

现在：

```c
tcp_timer_lcore(sk) = sk->owner_lcore;
```

worker 调用 `rte_timer_manage()`，因此：

```text
SYN/SYN+ACK RTO
数据 RTO
FIN RTO
TIME_WAIT 2MSL
```

都与 packet ingress、command 和 free 在同一 lcore 串行执行。

main lcore 仍可管理 ARP sweep 等不引用 TCB 的基础设施 timer。

timer callback 仍以 `nsock *` 为参数是安全的，因为它不会跨 owner 执行；`nsock_free()` 在释放前停止对应 timer。

### 十二、最终析构顺序

`nsock_free()` 只能从 owner lcore 调用。若调用者 lcore 不等于 `sk->owner_lcore`，函数记录错误并拒绝释放；选择泄漏并报警优于破坏 owner 正在使用的对象。

正常析构顺序：

```text
socket_owner_retire()
  ├─ abort 所有 waiter
  ├─ slots[id] = NULL
  └─ generation++

停止 TCP timer
释放 TCP sndbuf
从 TCP conn/listener/bind 或 UDP bind hash 删除
从 g_sock_list 删除
销毁 cond/mutex
释放 recv_buf/send_buf
rte_free(nsock)
```

先取消发布 handle，再释放对象内存，保证 owner 后续 dequeued 的旧命令只能得到 `EBADF`。

### 十三、并发 close 的可观察语义

若 SEND command 先于 CLOSE 被 owner 处理，SEND 可以成功；若 CLOSE 先处理，后续 SEND 因 `app_closed` 失败。这给共享 fd 的并发调用提供了明确的线性化顺序。

如果应用线程在 close 前已经复制 handle、但 command 在 close 后才到达：

- TCB 尚在 teardown：`app_closed` 拒绝命令；
- TCB 已释放：slot 为空；
- slot 已复用：generation 不匹配。

三种情况下都不会出现 UAF 或误操作新 socket。

### 十四、本次修改的模块边界

#### `socket_owner.h/.c`

- 定义 handle、command、owner slot 表和 command ring；
- 实现 adopt/retire、generation 校验；
- 实现 completion 和 recv/send/connect/accept waiter；
- 实现 owner command dispatcher。

#### `socket.h/.c`

- fd 表从裸指针改为 handle；
- BSD API 改为 command producer；
- `nsock` 增加 owner identity、可见/关闭状态和 waiter；
- `nsock_free()` 增加 owner 检查和 retire-before-free。

#### `tcp.c/.h`

- SEND/RECV/CONNECT/ACCEPT/CLOSE 改为 owner 非阻塞原语；
- ACK、窗口、payload、握手状态驱动 waiter；
- TCP timer 迁到 owner；
- 删除 `tcp_rx_events` 接收消费通知路径；
- listener close 区分未 accept 与已 accept child。

#### `udp.c`

- RECVFROM 改为 EAGAIN probe；
- ingress 直接唤醒 owner recv waiter；
- CLOSE 仅由 owner drain/free；
- 修复 unmatched UDP ingress 未释放 mbuf 的泄漏。

#### `main.c`

- app 启动前初始化 socket owner；
- packet worker 每轮处理 command；
- packet worker 管理 TCP timer。

#### `ring.h/.c` 与 `config.h`

- 删除 `tcp_rx_events` ring；
- 删除 `TCP_EVENT_RING_SIZE`。

#### `Makefile`

- 将 `socket_owner.c` 纳入构建。

### 十五、验证

本次实施完成后执行：

```text
pro-stack: make
pro-stack: CFLAGS="-Wall -Wextra" make
test: make test
IDE lints
```

结果：

- 完整静态构建通过；
- `-Wall -Wextra` 无警告；
- `test_rbtree: PASS`；
- `test_ofo: PASS`；
- IDE linter 无错误。

尚未完成真实 NIC 下的并发压力回归，验证重点见后续事项。

### 十六、保留的过渡实现与后续事项

#### 1. 删除 `nsock` 中遗留 mutex/cond

应用已不再使用 `sk->cond` 等待 socket 状态。TCP sndbuf 相关路径仍保留 `sk->mutex`，但 command、ACK、timer 和 TX 已由 owner 串行执行，后续应在确认所有调用点后删除这些锁和条件变量。

#### 2. dirty TX queue

`g_sock_list` 的遍历现在是 owner-local，因此生命周期安全，但每轮仍为 O(全部 socket 数)。应改为 socket 从“无发送工作”变为“有发送工作”时进入 dirty queue，只 flush 活跃 socket。

#### 3. 多 worker / RSS

当前只有全局 `g_owner`。handle 已保留 `owner_lcore`，后续应扩展为 per-worker owner context，并通过硬件 RSS 将同一四元组固定到同一 worker。协议 hash、timer、dirty queue 和 slot 表都应按 owner 分片。

#### 4. command 生命周期与取消

command 当前位于调用线程栈上，依赖调用者一直等待 completion。若加入超时、`pthread_cancel`、异步 coroutine 或真正 nonblocking API，应改为 slab/heap command，加引用计数、取消状态和 late-completion 处理。

#### 5. command ring 背压

当前 ring 满时 app lcore 使用 `rte_pause()` 忙等。应评估 per-app command ring、保留控制命令容量、eventfd/futex 或显式高低水位；CLOSE 等生命周期命令不能被丢弃。

#### 6. registry 分片

fd 表与 endpoint hash 仍共享 `registry_lock`。单 owner 下 hash 可转为 owner-local；多 worker 时应按 owner/RSS 分片，避免重新引入全局数据路径锁。

#### 7. 删除诊断 fd

`nsock->fd` 已不参与身份和生命周期，owner 创建的对象通常保持 `-1`，部分旧日志仍打印该值。日志应迁为 `{id,generation}`，之后删除该字段。

#### 8. 清理旧注释和 ring flags

部分注释仍描述 app 直接消费 `recv_buf` 或 app/worker 共同生产 `send_buf`。应按 owner 模型更新，并在确认 producer/consumer 唯一后收紧 DPDK ring flags。

#### 9. TCP 关闭策略

当前未实现 `SO_LINGER`、abortive close 和 FIN 入队失败时的 RST。需要明确 graceful close 与 abortive close 的 API 策略。

#### 10. 压力与竞态验证

至少覆盖：

- `nclose` 与 SEND/RECV 并发；
- fd 及 owner slot 高频复用；
- SYN_SENT timeout 与并发 close；
- RST 与 pending CONNECT/RECV/SEND；
- listener close 与 SYN_RECV/accept_queue/已 accept child；
- FIN_WAIT/LAST_ACK/TIME_WAIT timer 与回收；
- 多 app lcore 共享同一 fd；
- command ring 满载背压；
- UDP pending RECVFROM 与 close；
- ASan/UBSan（若 DPDK 构建环境允许）及长时间真实 NIC 流量。

### 结论

本次改动把 socket 生命周期从“跨 lcore 共享裸指针 + 局部加锁”转换为：

```text
fd
  → generation-checked handle
  → owner command ring
  → owner-local nsock / TCB
```

生命周期正确性不再依赖所有调用者都记得 pin 指针或持有同一把锁，而依赖更容易审计的结构不变量：**非 owner lcore 无法获得可解引用的 `nsock *`**。该边界也是后续 dirty queue、多 RX queue、RSS 和 per-worker 协议栈分片的基础。

---

## ARC-003：traffic-gen owner-local 队列与按需 TCP 缓冲

- **状态**：实施中。
- **范围**：`traffic-gen` owner-local flow、`owner_io`、`nsock`、TCP RX/TX 队列、发送缓冲和后续 per-worker 分片。
- **触发**：1000 CPS 短连接测试中，每个 socket 创建两个 DPDK ring；连接在 FIN_WAIT/TIME_WAIT 期间仍保留 ring，最终触发 DPDK memzone 段上限。
- **架构决策**：同一 owner worker 内的 traffic-gen socket 不创建 DPDK ring，改用嵌入 TCB 的 owner-local FIFO；DPDK ring 仅保留给跨 lcore 通信和 app-visible 兼容路径。

### 问题与证据

原路径中，一次 HTTP 短连接完成后，`tg_flow` 可立即归还对象池，但 TCP socket 还会经历 FIN_WAIT/TIME_WAIT。`nsock_free()` 之前，`recv_buf` 和 `send_buf` 一直占用两个 DPDK memzone。

```text
HTTP transaction complete
  → flow recycle
  → owner_io_close
  → FIN_WAIT / TIME_WAIT
  → nsock_free
  → rte_ring_free(recv_buf, send_buf)
```

因此 `max_concurrency` 只限制活跃 flow，不能限制仍在 TCP 关闭期的 ring 数。1000 CPS、约 2 秒关闭期可同时保留约数千个 socket，超过默认 memzone 描述符预算。

### 本次实施

1. 增加 `NSOCK_IO_OWNER_LOCAL` 模式；`traffic-gen` 使用 `owner_io_socket_create_local()` 创建该模式的 TCP socket。
2. TCP 收发路径统一通过 socket queue helper：
   - RX 队列保存 `tcp_rx_blob`；
   - TX 控制队列保存 `tcp_fragment`；
   - owner-local 模式使用嵌入 `tcp_stream` 的 FIFO，不调用 `rte_ring_create()`；
   - app-visible socket 保持原有 DPDK ring，避免改变 BSD API、UDP 和 echo app 语义。
3. `nsock_free()` 提供最终释放 observer；scheduler 分别记录活跃 transaction 与 `live_sockets`，后者仅在 TCP 完整释放后减少。
4. scheduler 停止、活跃 flow 与 `live_sockets` 均为零后，runtime 自动停止并等待 worker 退出。

### 长期内存策略

消除 ring 不是百万连接的终点。当前 `tcp_sndbuf_init()` 仍会为每个 TCP socket 预分配 `TCP_SNDBUF_SIZE`（64 KiB）；100 万 socket 仅该项就约 61 GiB。

后续应遵守：

1. TCB/flow pool 只保存四元组、序号、窗口、timer 和少量指针等固定元数据；
2. TX payload 改为按需申请的固定大小 chunk 链，仅保留未 ACK 数据并在 ACK 后归还；
3. traffic-gen 固定 HTTP 模板使用 template 引用、offset 和 lazy packet build；重传按模板重建，不复制完整请求；
4. `tcp_rx_blob`、OFO segment、`tcp_fragment` 从热路径 `rte_malloc` 迁为 per-worker mempool；
5. pool 耗尽时由 scheduler 背压并记录资源指标，不把本地资源不足误记为远端连接失败。

### 多核边界

`--workers N` 和多 queue/RSS 配置已建立基础，但当前 `socket_owner`、socket list、ARP 表、in/out ring 和 reactor 仍为进程全局单例。它们必须先按 worker 分片，才能安全启用多个协议 worker；在此之前，`--workers > 1` 必须 fail-closed。
