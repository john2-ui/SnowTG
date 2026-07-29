# DevLog

用于沉淀会影响后续实现、并发模型或模块边界的架构问题与决策。每一条记录独立编号，后续新增内容应追加为新的 `ARC-XXX` 条目，而不是混入已有条目。

## 记录索引

- [ARC-001：TCP 接收路径的跨 lcore 所有权缺陷](#arc-001tcp-接收路径的跨-lcore-所有权缺陷) — 接收路径已实施；生命周期收敛待后续处理

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
