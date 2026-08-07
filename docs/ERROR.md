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

### 当前约束

ARP 缓存现为 packet worker 独占的有界 `rte_hash`；ARP 老化和可选诊断
sweep 也都在该 worker 执行，因此不再与 main lcore 并发访问。多 worker
扩展前仍需将缓存按 worker 分片，不能重新把该单例暴露给多个协议 worker。

---



## 2. dpdk-pdump 抓包时 secondary 进程 PANIC（Cannot init tail queues for objects）



### 现象

主程序 `sudo ./build/dpdk_netarch` 正常运行后，另一终端启动抓包工具：

```bash
sudo dpdk-pdump -- \
  --pdump 'port=0,queue=*,rx-dev=/tmp/rx.pcap,tx-dev=/tmp/tx.pcap'
```

`dpdk-pdump`（secondary 进程）在 EAL 初始化阶段崩溃：

```text
EAL: Cannot initialize tailq: RTE_FIB
Tailq 0: qname:<RTE_RING>, ...
...
EAL: Cannot init tail queues for objects
EAL: PANIC in main():
Cannot init EAL
Aborted (core dumped)
```



### 原因

DPDK 多进程模型要求 primary 与 secondary 的 **tailq 集合一致**。每个 DPDK 库通过构造函数注册自己的 tailq（如 `RTE_FIB`、`RTE_LPM`、`RTE_ACL`），secondary 会按自身的完整库集去共享内存里查找这些 tailq。

`pkg-config --libs libdpdk` 的输出里带有 `-Wl,--as-needed`，链接器会把主程序**没有直接引用**的库全部丢弃。验证：

```bash
$ ldd build/dpdk_netarch-shared | grep -c librte
15                                  # 只链接了 15 个库
$ ldd build/dpdk_netarch-shared | grep -iE 'fib|lpm|acl'
                                    # 空：rte_fib/lpm/acl 未链接
```

主程序没链接 `rte_fib`，其构造函数不执行，共享内存里也就没有 `RTE_FIB` 的 tailq 槽位。而 `dpdk-pdump` 是完整版（注册了 `RTE_FIB`），挂载时找不到对应槽位，于是 `Cannot init tail queues for objects` 并 PANIC。

**本质：primary 链接的 DPDK 库集合比 secondary 少，tailq 对不上。**

### 解决方案

让主程序链接**完整**的 DPDK 库集，把链接标志里的 `--as-needed` 改成 `--no-as-needed`（`Makefile`）：

```make
# Force --no-as-needed so ALL DPDK libraries are linked, not just the ones the
# app references directly. 这样每个库的构造函数都会注册其 tailq，完整版
# secondary（如 dpdk-pdump）才能正常挂载。
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk | sed 's/-Wl,--as-needed/-Wl,--no-as-needed/')
```

重新编译后验证库集扩大且包含 `rte_fib`：

```bash
$ make clean && make
$ ldd build/dpdk_netarch-shared | grep -c librte
58                                  # 完整库集
$ ldd build/dpdk_netarch-shared | grep -iE 'fib|lpm|acl'
librte_fib.so.26 => ...
librte_lpm.so.26 => ...
librte_acl.so.26 => ...
```

之后重启主程序（先清理残留 socket）再挂 pdump 即可：

```bash
sudo rm -f /var/run/dpdk/rte/mp_socket*
sudo ./build/dpdk_netarch          # 终端 A
sudo dpdk-pdump -- --pdump 'port=0,queue=*,rx-dev=/tmp/rx.pcap,tx-dev=/tmp/tx.pcap'  # 终端 B
```



### 备注

- 该报错与 `--pdump` 参数无关，纯粹是 primary/secondary 库集不匹配。
- `--no-as-needed` 只能解决 tailq 缺失，让 secondary 通过 EAL 初始化；若
`dpdk-pdump` 为静态 DPDK 而 primary 为共享 DPDK，mempool ops 的注册顺序
仍可能不同并导致第 4 条中的崩溃。最终方案是让两者使用相同的静态链接模式。
- primary 与 secondary 必须是同一版本、同一 `--file-prefix`（本例默认均为 `rte`）。

---



## 3. dpdk-pdump 写 pcap 文件失败（Failed to hotplug add device / create_mp_ring_vdev）



### 现象

解决第 2 条后，`dpdk-pdump` 不再 PANIC，但仍在创建 vdev 时退出：

```text
EAL: Failed to hotplug add device
EAL: Error - exiting with code: 1
vdev creation failed:create_mp_ring_vdev:695
```



### 原因

抓包参数使用了写文件形式 `rx-dev=/tmp/rx.pcap`。pdump 对「文件」类型会创建一个 `net_pcap` vdev 来写 pcap：

```c
(pt->rx_vdev_stream_type == IFACE) ?
snprintf(vdev_args, ... VDEV_IFACE_ARGS_FMT, pt->rx_dev) :
snprintf(vdev_args, ... VDEV_PCAP_ARGS_FMT, pt->rx_dev);   /* 文件 -> net_pcap */
if (rte_eal_hotplug_add("vdev", vdev_name, vdev_args) < 0) { ... }
```

但当前 DPDK **没有编译** `net_pcap` **PMD**：

```bash
$ ls /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-*/ | grep -i pcap
                                    # 空：只有 net_ring，没有 net_pcap
$ dpkg -l | grep -iE 'libpcap.*dev'
                                    # 空：libpcap-dev 未安装
$ ldconfig -p | grep -i libpcap
libpcap.so.0.8 => ...               # 只有运行时库，缺开发头文件
```

DPDK 用 meson 编译时检测不到 `libpcap-dev`，就跳过了 pcap 驱动。因此创建 `net_pcap` vdev 时 `rte_eal_hotplug_add()` 失败。

**本质：缺少** `net_pcap` **PMD，无法把镜像流量写成 pcap 文件。**（primary/secondary 之间的传输环 `net_ring` 是存在的，只差 pcap 输出这一环。）

### 解决方案

安装 `libpcap-dev` 后重新编译 DPDK，让 meson 启用 pcap 驱动：

```bash
sudo apt update
sudo apt install libpcap-dev

cd ~/dpdk
meson setup --reconfigure build      # 重新检测依赖，启用 pcap 驱动
ninja -C build                       # 增量编译，只多编 net_pcap
sudo ninja -C build install
sudo ldconfig
```

验证 `net_pcap` PMD 出现：

```bash
$ ls /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-*/ | grep pcap
librte_net_pcap.so ...
```

之后重启主程序并重新抓包：

```bash
sudo rm -f /var/run/dpdk/rte/mp_socket*
sudo ./build/dpdk_netarch                                              # 终端 A
sudo dpdk-pdump -- \
  --pdump 'port=0,queue=*,rx-dev=/tmp/rx.pcap,tx-dev=/tmp/tx.pcap'     # 终端 B
```



### 备注

- 若暂不想重编 DPDK，可改用宿主机 Wireshark 抓桥接网卡（WLAN），不依赖 pdump。
- 重编只是新增一个驱动，增量 `ninja` 很快，不会从头编译整个 DPDK。

---



## 4. 启动 dpdk-pdump 后 primary 在 mempool CNXK PMD 中段错误



### 现象

主程序单独运行正常；`dpdk-pdump` 成功创建 pcap vdev 并挂载后，主程序立即
`Segmentation fault (core dumped)`，pdump 随后发现 primary 消失：

```text
# primary
[INFO ][udp.c:75] udp 192.168.21.105:47584 -> 192.168.21.255:47584 len=69
Segmentation fault (core dumped) sudo ./build/dpdk_netarch

# secondary
Port 1 MAC: 02 70 63 61 70 00
Port 2 MAC: 02 70 63 61 70 01
Primary process is no longer active, exiting...
```

内核日志明确指出崩溃位置：

```text
dpdk_netarch: segfault at 43c00 ... in librte_mempool_cnxk.so.26.2
```



### 原因

两进程的 DPDK 链接模式不一致：

```text
primary:   EAL: Detected shared linkage of DPDK
secondary: EAL: Detected static linkage of DPDK
```

第 2 条为了补齐 tailq，把共享 primary 改为 `--no-as-needed`，因此所有共享 PMD
都被加载，EAL 初始化可以通过。但 DPDK mempool ops 使用进程内注册索引；静态和
共享链接时 PMD 构造函数的加载/注册顺序可能不同。

pdump 在静态 secondary 中创建共享 mempool，其 ops 索引被 primary 按自己的
共享 PMD 注册表解释。同一个索引在 primary 中错误地对应到 `mempool_cnxk`。
RX/TX pdump 回调调用 `rte_pktmbuf_copy()` 从该共享池分配 mbuf 时，最终进入并不
适用于当前普通内存池的 CNXK 操作函数，产生接近 NULL 的非法访问并使 primary
崩溃。

```c
/* pdump 回调：启用抓包后，每个包都会走到这里 */
p = rte_pktmbuf_copy(pkts[i], mp, 0, cbs->snaplen);
```

**本质：primary/secondary 虽是同一 DPDK 版本，但链接模式和 PMD 注册顺序不一致，
共享 mempool 的 ops 索引被错误解释。**

### 解决方案

安装的 `dpdk-pdump` 使用静态 DPDK，因此主程序也改为静态 DPDK，并将静态目标设为
默认构建：

```make
# 与安装版 dpdk-pdump 使用相同的 DPDK 链接模式。
all: static

# pkg-config 已用 -l:librte_*.a 指定 DPDK 静态库。
# 不要全局加 -Bstatic，否则 libc/libgcc/systemd 等系统库也会被强制静态链接。
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)
```

重新构建：

```bash
make clean
make
readlink build/dpdk_netarch
# 期望：dpdk_netarch-static
```

然后清理旧 socket，依次启动：

```bash
# 终端 A
sudo rm -f /var/run/dpdk/rte/mp_socket*
sudo ./build/dpdk_netarch

# 终端 B
sudo dpdk-pdump -- \
  --pdump 'port=0,queue=*,rx-dev=/tmp/rx.pcap,tx-dev=/tmp/tx.pcap'
```



### 验证说明

静态 DPDK 主程序已成功编译，`build/dpdk_netarch` 已指向
`dpdk_netarch-static`。自动运行联调因当前非交互终端无法提供 sudo 密码而未执行，
需要在用户终端按上述命令完成运行时验证。