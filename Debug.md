# DPDK 程序调试方法与案例

本文面向正在学习 DPDK 的开发者，介绍如何从日志、内核信息、Core Dump、
二进制链接关系和 DPDK 多进程约束中定位问题。文中的案例来自
`06_netarch` 项目开发过程；具体错误记录另见 `06_netarch/Error.md`。

---

## 1. 调试的基本原则

### 1.1 先收集证据，再修改代码

面对 `Segmentation fault`、EAL 初始化失败或抓包工具异常时，不要仅根据最后一条
业务日志判断崩溃位置。最后一条日志只说明程序在它之后崩溃，不代表打印日志的
函数就是根因。

推荐顺序：

1. 保存程序和辅助进程的完整输出。
2. 查看内核记录的崩溃地址与共享库。
3. 获取 Core Dump 和线程回溯。
4. 检查可执行文件的版本、链接模式和依赖库。
5. 根据证据建立假设。
6. 设计最小改动验证假设。
7. 修复后同时验证原功能和故障场景。

### 1.2 区分事实、推断和结论

例如：

- **事实**：内核报告崩溃指令位于 `librte_mempool_cnxk.so`。
- **事实**：primary 是 shared DPDK，secondary 是 static DPDK。
- **推断**：两进程的 mempool ops 注册顺序不一致。
- **验证方式**：统一链接模式后重新运行相同抓包场景。

没有完整回溯时，应把原因称为“高概率推断”，而不是未经验证的绝对结论。

### 1.3 一次只改变一个关键变量

如果同时修改链接方式、内存池大小、CPU 参数和业务代码，即使问题消失，也无法
确认是哪项修改起作用。调试时应尽量使用最小实验：

```text
原场景：shared primary + static pdump -> 崩溃
实验：  static primary + static pdump -> 观察是否仍崩溃
```

---

## 2. 建立故障时间线

同时运行 primary 和 secondary 时，至少保留两个终端的输出：

```bash
# 终端 A：主程序
sudo ./build/dpdk_netarch 2>&1 | tee /tmp/netarch.log

# 终端 B：抓包程序
sudo dpdk-pdump -- \
  --pdump 'port=0,queue=*,rx-dev=/tmp/rx.pcap,tx-dev=/tmp/tx.pcap' \
  2>&1 | tee /tmp/pdump.log
```

重点记录：

- 哪个进程先退出。
- 辅助进程退出前是否已成功连接 primary。
- 是否出现端口、ring、mempool 或 vdev 创建成功的日志。
- primary 最后一条业务日志是什么。
- 问题是否只在启用某项功能后出现。

本项目的一个关键现象是：

```text
dpdk-pdump:
Port 1 MAC: 02 70 63 61 70 00
Port 2 MAC: 02 70 63 61 70 01
Primary process is no longer active, exiting...
```

它表示 pdump 已创建 pcap vdev，但随后检测到 primary 消失。因此真正崩溃的是
primary，而不是 pdump 主动退出导致抓包停止。

---

## 3. 查看 Linux 内核崩溃日志

### 3.1 使用 journalctl

查询最近的内核日志：

```bash
journalctl -k --since "20 minutes ago" --no-pager
```

只看目标程序和段错误：

```bash
journalctl -k --since "20 minutes ago" --no-pager |
  rg -i 'dpdk_netarch|segfault'
```

本项目得到过：

```text
dpdk_netarch: segfault at 43c00
ip ... in librte_mempool_cnxk.so.26.2
likely on CPU 0
```

如何解读：

- `segfault at 43c00`：被访问的虚拟地址。地址很小通常意味着 NULL 指针加偏移，
  但仍需回溯确认。
- `ip ...`：发生异常时的指令地址。
- `in librte_mempool_cnxk.so`：崩溃指令属于哪个二进制或共享库。
- `CPU 0`：发生故障的 CPU；在 DPDK 中可用于对应主 lcore/worker lcore。
- `error 4`：x86 页错误信息之一，通常表示用户态读取不存在页面。

### 3.2 dmesg 作为替代

```bash
sudo dmesg -T | rg -i 'segfault|dpdk_netarch'
```

某些系统限制普通用户读取 `dmesg`，此时优先使用 `journalctl -k`。

### 3.3 为什么内核日志重要

业务日志可能停在 UDP、ARP 或 ICMP 处理函数，但内核日志能指出真正执行崩溃指令
的库。本案例最后打印的是 UDP 日志，实际崩溃却发生在 mempool PMD，因此避免了
错误地只检查 `udp.c`。

---

## 4. Core Dump 与 GDB

内核日志只能提供有限信息。最可靠的方法仍是分析 Core Dump。

### 4.1 检查 Core Dump 设置

```bash
ulimit -c
cat /proc/sys/kernel/core_pattern
```

- `ulimit -c` 为 `0`：当前 shell 禁止生成传统 core 文件。
- `core_pattern` 以 `|` 开头：崩溃信息会交给 apport/systemd-coredump 等程序。

临时允许生成：

```bash
ulimit -c unlimited
```

### 4.2 使用 systemd-coredump

若系统安装了对应组件：

```bash
coredumpctl list dpdk_netarch
coredumpctl info dpdk_netarch
sudo coredumpctl gdb dpdk_netarch
```

如果提示命令不存在：

```bash
sudo apt install systemd-coredump
```

### 4.3 GDB 常用命令

进入 GDB 后：

```gdb
bt
bt full
info threads
thread apply all bt full
frame 0
info registers
disassemble /m
```

DPDK 是多线程轮询程序，不能只看当前线程，推荐：

```gdb
thread apply all bt full
```

### 4.4 调试构建

发布构建常使用 `-O3`，变量可能被优化掉。需要回溯时可临时使用：

```make
CFLAGS += -O0 -g3 -fno-omit-frame-pointer
```

重新构建后再复现。不要用调试构建做性能结论。

### 4.5 使用 addr2line

已知二进制内偏移时：

```bash
addr2line -Cfipe build/dpdk_netarch-static 0x地址
```

共享库启用了 ASLR 时，需要先把运行时地址减去库的加载基址，得到库内偏移。

---

## 5. 检查二进制与链接关系

DPDK 多进程问题经常不是业务代码错误，而是 primary/secondary 构建环境不一致。

### 5.1 查看文件类型

```bash
file build/dpdk_netarch
file "$(command -v dpdk-pdump)"
```

### 5.2 查看动态依赖

```bash
ldd build/dpdk_netarch
ldd "$(command -v dpdk-pdump)"
```

关注：

- 是否链接 `librte_pdump`、`librte_mempool`、`librte_ring`。
- primary 是否加载了 secondary 不同的一组 PMD。
- 是否同时使用 `/usr/lib` 和 `/usr/local/lib` 中不同版本的 DPDK。

统计动态 DPDK 库数量：

```bash
ldd build/dpdk_netarch | rg -c 'librte'
```

### 5.3 查看 DPDK 版本与编译参数

```bash
pkg-config --modversion libdpdk
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
pkg-config --static --libs libdpdk
```

### 5.4 从 EAL 日志看链接模式

DPDK 启动时会直接打印：

```text
EAL: Detected shared linkage of DPDK
```

或：

```text
EAL: Detected static linkage of DPDK
```

比较 primary 与 secondary 的这两行，是排查 DPDK 多进程问题的重要步骤。

---

## 6. DPDK 多进程调试检查表

primary 和 secondary 至少应满足：

1. 使用相同 DPDK 版本和 ABI。
2. 使用相同的 `--file-prefix`。
3. 能访问相同 hugepage 文件和运行目录。
4. 使用兼容的虚拟地址布局与 IOVA 模式。
5. 加载一致的共享对象、tailq 和 mempool ops。
6. 最好使用相同的 DPDK 静态/共享链接模式。
7. PMD 构造函数的注册集合与顺序必须兼容。

检查运行目录：

```bash
ls -la /var/run/dpdk/rte/
```

清理残留 socket 前应确保没有 primary 仍在运行：

```bash
pgrep -a dpdk_netarch
sudo rm -f /var/run/dpdk/rte/mp_socket*
```

不要在 primary 正常运行时删除它正在使用的 socket。

---

## 7. 案例一：Cannot initialize tailq: RTE_FIB

### 7.1 现象

```text
EAL: Cannot initialize tailq: RTE_FIB
EAL: Cannot init tail queues for objects
EAL: PANIC in main(): Cannot init EAL
```

### 7.2 调查

主程序使用：

```make
LDFLAGS_SHARED = $(shell pkg-config --libs libdpdk)
```

其中包含 `-Wl,--as-needed`，链接器只保留主程序直接引用的共享库。检查发现：

```bash
ldd build/dpdk_netarch-shared | rg -c 'librte'
# 只有约 15 个

ldd build/dpdk_netarch-shared | rg -i 'fib|lpm|acl'
# 无输出
```

而完整版 `dpdk-pdump` 注册了 `RTE_FIB` 等对象。secondary 在共享 tailq 中找不到
对应槽位，因此 EAL 初始化失败。

### 7.3 临时修复

使用 `--no-as-needed` 可以让 shared primary 加载完整 DPDK 库集：

```make
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk | \
  sed 's/-Wl,--as-needed/-Wl,--no-as-needed/')
```

这解决了 tailq 缺失，但它只是阶段性修复：如果 secondary 是静态链接，后续仍
可能因 PMD 注册顺序不同而崩溃。

### 7.4 方法总结

EAL 报某个 tailq 不存在时：

1. 不要只检查业务代码。
2. 比较 primary/secondary 的库集合。
3. 检查 `--as-needed` 是否裁剪了带构造函数的库。
4. 继续验证链接模式和 PMD 注册顺序，而不是见到 EAL 能启动就认为完全修复。

---

## 8. 案例二：Failed to hotplug add device

### 8.1 现象

```text
EAL: Failed to hotplug add device
vdev creation failed:create_mp_ring_vdev
```

### 8.2 调查

pdump 参数要求输出 pcap 文件：

```bash
--pdump 'port=0,queue=*,rx-dev=/tmp/rx.pcap'
```

源码显示文件输出需要创建 `net_pcap` vdev。检查 PMD：

```bash
ls /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-*/ | rg -i 'pcap|ring'
```

系统只有 `net_ring`，没有 `net_pcap`；同时没有安装开发依赖：

```bash
dpkg -l | rg 'libpcap.*dev'
```

### 8.3 修复

```bash
sudo apt install libpcap-dev
cd ~/dpdk
meson setup --reconfigure build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

### 8.4 方法总结

`hotplug add device` 失败时应确认：

1. 要创建的 vdev 名称和参数。
2. 对应 PMD 是否存在。
3. PMD 的外部开发依赖是否在 DPDK 编译前安装。
4. 安装后是否执行 `ldconfig`。

---

## 9. 案例三：启动 pdump 后 primary 在 mempool_cnxk 中崩溃

### 9.1 证据链

1. 主程序单独运行正常。
2. pdump 创建 pcap vdev 成功。
3. pdump 启用抓包后 primary 立即崩溃。
4. 内核指出崩溃位于 `librte_mempool_cnxk.so`。
5. 当前硬件是 VMware VMXNET3，不应使用 CNXK mempool。
6. primary 显示 shared linkage，pdump 显示 static linkage。

### 9.2 pdump 启用后发生了什么

pdump 会给端口安装 RX/TX 回调。每个经过的包都会被复制到 secondary 创建的共享
mempool：

```c
p = rte_pktmbuf_copy(pkts[i], mp, 0, cbs->snaplen);
```

mempool 对象中包含 ops 索引，而具体 ops 注册表是进程内建立的。静态与共享链接
加载 PMD 的顺序不一致时，同一索引可能在两个进程中指向不同实现。

因此，本案例的高概率原因是：

```text
secondary 创建的普通 ring mempool
        ↓ 共享 ops 索引
primary 按自己的注册顺序解释
        ↓
误调用 mempool_cnxk 操作
        ↓
非法内存访问
```

### 9.3 最终修复方向

使 primary 与安装版 static `dpdk-pdump` 使用相同的 DPDK 静态链接方式：

```make
all: static

# pkg-config 已用 -l:librte_*.a 指定 DPDK 静态库。
# 不应全局添加 -Bstatic，否则系统依赖也被强制静态链接。
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)
```

错误写法：

```make
LDFLAGS_STATIC = -Wl,-Bstatic $(shell $(PKGCONF) --static --libs libdpdk)
```

它会要求 `libgcc_s`、glibc、systemd、libpcap 等全部提供静态版本，常见结果是：

```text
cannot find -lgcc_s
```

正确方案是“DPDK 静态、系统依赖保持动态”，而不是生成完全静态的 ELF。

### 9.4 验证步骤

```bash
make clean
make
readlink build/dpdk_netarch
# 应指向 dpdk_netarch-static
```

运行：

```bash
# 终端 A
sudo rm -f /var/run/dpdk/rte/mp_socket*
sudo ./build/dpdk_netarch

# 终端 B
sudo dpdk-pdump -- \
  --pdump 'port=0,queue=*,rx-dev=/tmp/rx.pcap,tx-dev=/tmp/tx.pcap'
```

检查：

- primary 是否持续运行。
- pdump 是否持续增长 `packets dequeued`。
- `/tmp/rx.pcap`、`/tmp/tx.pcap` 是否增长。
- Wireshark 是否能正常打开文件。

如果仍崩溃，应生成 Core Dump，用 GDB 验证是否确实经过
`rte_pktmbuf_copy()` 和错误的 mempool ops，而不是继续只凭推断修改。

---

## 10. DPDK 业务代码自身的常见检查

外部工具触发崩溃不代表一定是 DPDK 库问题，也要检查业务代码。

### 10.1 检查包长

读取协议头前必须验证：

- `rte_pktmbuf_pkt_len()` 是否至少包含 Ethernet/IP/UDP 头。
- IPv4 IHL 是否合法。
- UDP `dgram_len >= sizeof(struct rte_udp_hdr)`。
- UDP 长度是否不超过 mbuf 中实际数据。

否则：

```c
uint16_t payload_len = dgram_len - sizeof(struct rte_udp_hdr);
```

可能发生无符号下溢，随后 `rte_memcpy()` 越界。

### 10.2 检查 ring enqueue 返回值

```c
unsigned int enqueued =
    rte_ring_sp_enqueue_burst(ring->in, (void **)rx, nb_rx, NULL);

for (unsigned int i = enqueued; i < nb_rx; i++)
        rte_pktmbuf_free(rx[i]);
```

忽略返回值会在 ring 满时泄漏未入队的 mbuf。

### 10.3 明确 mbuf 所有权

- `rte_eth_rx_burst()` 返回后：应用拥有 mbuf。
- 成功入 ring 后：所有权转给消费者。
- `rte_eth_tx_burst()` 成功提交的 mbuf：驱动拥有，应用不能立即释放。
- `rte_eth_tx_burst()` 未提交的尾部 mbuf：仍由应用释放或重试。
- pdump 克隆包拥有独立 mbuf，不应干扰原包生命周期。

---

## 11. 推荐的完整调试流程

```text
1. 稳定复现
   ↓
2. 保存 primary/secondary 完整日志
   ↓
3. journalctl -k 确认真正崩溃模块
   ↓
4. Core Dump + GDB 获取所有线程回溯
   ↓
5. file/ldd/pkg-config 对比版本与链接方式
   ↓
6. 检查 DPDK file-prefix、hugepage、PMD、tailq、mempool ops
   ↓
7. 建立最小假设，只改一个关键变量
   ↓
8. 重复原故障场景验证
   ↓
9. 回归 ARP/ICMP/UDP 和抓包功能
   ↓
10. 把现象、证据、根因、修复、验证写入 Error.md
```

---

## 12. 问题记录模板

每个问题建议按以下格式记录：

```markdown
## N. 问题标题

### 现象
- 执行命令
- 完整关键日志
- 哪个进程先退出

### 环境
- DPDK 版本
- 内核版本
- 网卡/PMD
- primary/secondary 链接模式

### 证据
- 内核日志
- Core Dump 回溯
- file/ldd/pkg-config 结果

### 原因
- 已证实事实
- 推断过程
- 尚未确认的部分

### 解决方案
- 修改前代码
- 修改后代码
- 为什么有效

### 验证
- 构建结果
- 运行结果
- 回归测试

### 遗留风险
- 并发、资源释放、兼容性或性能问题
```

良好的调试记录不仅说明“改了什么”，还应保留“为什么这样判断”和“如何证明修复
有效”，这样相似问题再次出现时才能快速复用经验。
