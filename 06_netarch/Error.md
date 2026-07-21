# 开发问题记录（Error.md）

记录开发本 DPDK 网络协议栈（`06_netarch`）过程中遇到的问题、原因分析与解决方案。

---

## 1. 多线程惰性初始化共享单例导致启动即段错误（Segmentation fault）

### 现象

程序 `sudo ./build/dpdk_netarch` 启动后，各初始化日志正常打印，但在 worker 线程刚启动的瞬间崩溃：

```text
[INFO ][port.c:49] port 0 started (driver=net_vmxnet3)
[INFO ][net_context.c:14] local identity: port=0 ip=192.168.21.2 mac=00:0c:29:f1:e6:6a
[INFO ][ring.c:28] rx/tx rings created (size=1024)
[INFO ][netarch.c:85] packet worker started on lcore 1
Segmentation fault         (core dumped) sudo ./build/dpdk_netarch
```

### 原因

共享单例（收发环 `inout_ring`、ARP 表）采用「首次使用时惰性创建」，且 `main` 与 worker 两个 lcore 会同时调用。

`ring_instance()` 内部先把全局指针置为非空，再逐个创建 `in`/`out` 环，存在竞态窗口：

```c
struct inout_ring *ring_instance(void) {
        if (r_instance != NULL)
                return r_instance;              /* (B) 另一线程可能在此提前返回 */

        r_instance = rte_malloc("inout_ring", sizeof(struct inout_ring), 0);
        /* (A) 此刻 r_instance 已非空，但 in/out 仍为 NULL */
        memset(r_instance, 0, sizeof(struct inout_ring));
        r_instance->in = rte_ring_create("in_ring", RING_SIZE, rte_socket_id(), 0);
        r_instance->out = rte_ring_create("out_ring", RING_SIZE, rte_socket_id(), 0);
        ...
}
```

- 线程 A（worker）执行到 (A)：指针已非空，但 `in`/`out` 尚未建好。
- 线程 B（main）在 (B) 看到指针非空，直接返回这个「半成品」。
- 由于网卡是**桥接模式**，开机即有大量广播帧，`main` 的收包循环立刻命中：

```c
unsigned int nb_rx = rte_eth_rx_burst(g_net.port_id, 0, rx, BURST_SIZE);
if (nb_rx > 0)
        rte_ring_sp_enqueue_burst(ring->in, (void **)rx, nb_rx, NULL);
        /* ring->in 可能仍为 NULL -> 解引用崩溃 */
```

即在 `ring->in` 还是 NULL 时就被使用，触发段错误。日志停在 `packet worker started`，正好是两线程同时运行、第一批包到达的时刻。

### 解决方案

在**启动 worker 之前**，于主 lcore 上一次性创建所有共享单例，消除竞态：

```c
const uint16_t port_id = 0;
port_init(port_id, mp);
net_context_init(port_id, g_local_ip);

/* 在 rte_eal_remote_launch() 之前完成共享单例创建，避免两 lcore 竞态 */
struct inout_ring *ring = ring_instance();
arp_table_instance();

/* ... 定时器设置 ... */

rte_eal_remote_launch(pkt_worker, mp,
                      rte_get_next_lcore(rte_lcore_id(), 1, 0));
```

这样 worker 启动时，`in`/`out` 环与 ARP 表均已完整初始化。

### 遗留隐患

ARP 表链表仍会被 worker（收到 ARP 回复时写入）与定时器回调 `arp_sweep_cb`（主 lcore 读取）并发访问，目前未加锁。学习场景一般不触发，若后续出现偶发崩溃或表数据错乱，需要给 ARP 表加锁或改为单线程访问。
