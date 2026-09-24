# SnowTG

## 项目介绍

`SnowTG` 是一个基于 DPDK 的用户态 IPv4 协议栈与混合流量发生器。项目以单 owner、per-core reactor 和无锁热路径为核心，提供 TCP/UDP socket 能力，并用自研协议栈驱动 HTTP/DNS 压测流量。

English documentation: [`README-en.md`](README-en.md)

仓库主要包含：

- [`pro-stack/`](pro-stack/)：用户态 Ethernet/ARP/IPv4/ICMP/TCP/UDP 协议栈，提供 BSD 风格 API 和 owner-local 非阻塞接口。
- [`traffic-gen/`](traffic-gen/)：剧本驱动的 HTTP/1.1、DNS 混合流量发生器，支持 CPS、并发水位、连接复用、分片调度和 CSV 指标。
- [`apps/`](apps/)：TCP/UDP echo 示例与协议栈运行入口。
- [`test/`](test/)：协议、owner 生命周期、调度器、场景解析和统计等回归测试。

项目架构与后续工作见 [`docs/TODO.md`](docs/TODO.md)，性能测试记录见 [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md)。

## 吞吐演进

以下展示截至 **2026-09-17** 的代表性 HTTP 成功 RPS 记录。横轴按测试阶段排列，
纵轴采用**对数刻度**，便于同时观察百级到十万级吞吐的变化。

![SnowTG HTTP 吞吐演进：短连接与 Keep-Alive 成功 RPS，包含历史回落及测试平台切换](docs/assets/throughput-history.svg)

**09-16 同时更换被压端并采用 worker TX**：发流端仍是 DPDK 虚拟机，被压端由
2 vCPU 虚拟机切换到 NUC，收发路径为 **Main RX + worker TX**。

各阶段的机器、并发与统计窗口存在差异，虚线标出平台或口径切换，不能将整条曲线
解释为同环境下的纯代码优化收益。09-17 使用重复测试均值：短连接 **114,302 RPS**
（两轮全程零失败），Keep-Alive **370,096 RPS**（两轮全程失败合计 **152**）。

<details>
<summary>查看图中数据与统计口径</summary>

图中标签取整，下表保留原记录精度；“—”表示该阶段未选取 Keep-Alive 记录。
数据来源：[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) §3.1～3.6、§3.8。

| 日期 | 阶段 | 短连接成功 RPS | Keep-Alive 成功 RPS | 条件与口径 |
| --- | --- | ---: | ---: | --- |
| 08-05 | 优化前 | 628.99 | — | 并发 100，目标 1,000 CPS，原日志记录 |
| 08-05 | owner-local FIFO | 888.34 | — | 并发 100，目标 1,000 CPS，120 秒 |
| 08-06 | 按需内存（ARC-003） | 1,024.45 | — | 并发 100，目标 10,000 CPS，120 秒 |
| 08-08 | Dirty TX | 2,617.60 | — | 4 workers / 并发 1,000，按 120 秒及 worker 计数汇总 |
| 08-12 | 并发 500 复测 | 4,216.22 | — | 8 workers / 并发 500，目标 15,000 CPS，120 秒 |
| 08-13 | 连接复用对照 | 3,953.43 | 14,396.23 | 8 workers / 并发 500，累计成功数 / 120 秒 |
| 09-16 | VM → NUC，Main RX + worker TX | 11,139 | 27,221 | 短连接 8w/256；KA 8w/1,000；各 3×30 秒稳态均值 |
| 09-17 | NUC → HP 推荐配置 | 114,302 | 370,096 | 短连接 1w/512；KA 2w/512；各 2×30 秒稳态均值 |

08-13 Keep-Alive 使用最后一个完整的 8-worker 累计快照，缺少完整 final/排空记录。
08-08 的 8 workers 记录受当天较低的端到端延迟影响，未纳入曲线，避免将环境差异视为多 worker 优化收益。
09 月稳态统计排除启动约 5 秒及末尾约 2 秒，全程失败另计；图中未使用单次探索峰值。

09-16 在同一 NUC 对端、同一二进制、8 workers / 并发 500 下，仅将 Main TX 改为
worker TX，短连接由 **8,280 → 11,161 RPS（+34.80%）**，Keep-Alive 由
**19,596 → 27,262 RPS（+39.12%）**，每组均重复 3×30 秒。
这组对照用于衡量 worker TX 的收益，并发与图中推荐配置不同。

更换被压端的依据：旧虚拟机同期 CPU 达 95%–100%，短连接的 CPU1 几乎被软中断占满；
发流端 RX 满 burst 比例仅 0.006%–0.035%，RX/TX 软件丢包及有效 NIC RX missed 增量均为 0。
在旧对端上切换 worker TX 的三轮均值变化仅为 +2.12% / −1.17%，未显示稳定增益。
这些证据支持旧被压虚拟机的 CPU / 虚拟化网络路径限制吞吐；不能仅凭 DPDK 发流端
CPU 100% 判断其饱和，因为它包含空轮询。详见[本地实验数据索引](debug/README.md)。

</details>

## 测试机器

以下配置核对于 2026-09-16～09-17；虚拟机 CPU 型号为 guest 识别值，vCPU 不代表独占物理核，内存为 Linux 可见容量。

| 角色 | 机器型号 / CPU | CPU 与内存 | 压测网卡 | 用途与限制 |
| --- | --- | --- | --- | --- |
| DPDK 压测虚拟机（`snow`） | VMware Virtual Platform，Workstation 17.6；Intel Core Ultra 9 185H | 16 vCPU / 7.20 GiB | VMXNET3（`vmxnet3`，桥接） | 运行 SnowTG；当前虚拟接收路径实测全部进入 RX0，推荐 Main RX + worker TX |
| 初始被压虚拟机（`192.168.21.106`） | VMware Virtual Platform；Intel Core Ultra 9 185H | 2 vCPU / 3350 MiB | 虚拟 Intel 网卡（`e1000`） | 早期 HTTP/DNS 对端；HTTP 压测时 CPU 达 95%–100%，限制吞吐对比 |
| NUC 裸机（`192.168.10.86`） | Intel NUC12WSKi5；Intel Core i5-1240P | 12 核 / 16 线程，15.6 GiB | Intel I225-V（`igc`），最高 2.5 Gbps，当前链路 1 Gbps | 可作为 HTTP/DNS 对端或 DPDK 压测端；VFIO 与硬件 RSS 4 RX + 4 TX 已验证 |
| 新被压裸机（`192.168.10.234`） | HP Laptop 15-fc0xxx；AMD Ryzen 7 7730U | 8 核 / 16 线程，15322 MiB | Realtek RTL8153 USB 3 千兆网卡（`r8152`），1 RX / 1 TX | nginx HTTP / dnsmasq DNS；iperf3 正向、反向分别约 938 / 942 Mbps，小包能力需结合单核软中断判断 |

初始被压机最初按“2 核 / 2 GB”提供，表中内存采用后续实测的 3350 MiB。
NUC → HP 最高实测：HTTP 短连接 **118,313 RPS**（4w/并发2048，单次20秒，全程失败2048）；
Keep-Alive **370,129 RPS**（2w/并发512，单次30秒，全程失败7），接近千兆发送线速。
短连接推荐 **1w/并发512：两轮均值114,302 RPS、两轮全程零失败**。
以上均为稳态成功 RPS，对端启用软件 RPS、每 owner 容量16384；峰值与修复后推荐值的版本、完整配置及证据见
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md#38-2026-09-17--nuc-发流hp-裸机接收)。尚未测得 NUC 的 CPU 上限。

双机源码工作区为 VM 的 `/home/snow/dpdk-l` 和 NUC 的 `/home/lca/work/dpdk-l`。
后续代码修改同步到两端，分别构建、验证；源码同步排除编译产物和压测数据，实验快照另行归档。
NUC 构建前执行 `source /home/lca/work/snowtg-env.sh`，使用安装在
`/opt/dpdk-26.07-rc3` 的 DPDK。NUC 已验证 VFIO Type 1 和 DPDK 四队列 UDP RSS；
默认自协商多数为 1 Gbps，但仍偶发 100 Mbps；压测逐秒记录链路，异常样本单独保留并排除对比。
程序启动先等待链路就绪（最多 20 秒），避免将 PHY 协商时间计入发流阶段。

## 使用方法

### 构建

构建前需安装 DPDK 及项目依赖，并准备可用的巨页和 DPDK 网卡。各目标可独立构建：

```bash
# 协议栈静态库：pro-stack/build/libpro-stack.a
make -C pro-stack

# TCP/UDP echo 示例：apps/stack-demo/build/stack-demo
make -C apps/stack-demo

# 混合流量发生器：traffic-gen/build/traffic-gen
make -C traffic-gen

# 构建并运行测试
make -C test
```

需要检查内存或未定义行为时，可在独立构建目录运行 ASan/UBSan：

```bash
./run-sanitizers.sh
# 可选：CC=clang JOBS=8 ./run-sanitizers.sh
```

### Docker 一键构建与自检

安装 Docker Engine 后，在仓库根目录执行（需要至少两个可用 CPU）：

```bash
docker build -t snowtg . && docker run --rm --network none snowtg
```

镜像基于 Ubuntu 26.04，下载并校验 SHA-256 后从源码构建 DPDK 26.07，再编译
`traffic-gen` 和 `stack-demo`，并在构建时执行 `make -C test`；任一步骤失败都会
终止构建。固定 DPDK 版本是为了满足重复 IPv4 分片重组等回归测试的行为要求。
`.dockerignore` 排除宿主机编译产物和压测数据，避免混用本机 DPDK。
镜像内使用共享 DPDK 库，默认包含 null、pcap、AF_PACKET、VMXNET3、virtio、
Intel e1000/igc PMD，覆盖项目现有的 VM 和 NUC 网卡。保留构建工具和测试，方便
容器内复测。其他网卡可通过 `--build-arg 'DPDK_DRIVERS=bus/*,common/*,mempool/*,net/*'`
启用全部可构建的网络驱动；如使用 mlx5，还需在 Dockerfile 中添加其开发依赖。
默认以 4 个任务编译，可用
`docker build --build-arg JOBS=8 -t snowtg .` 调整。DPDK 使用 generic CPU 配置，
避免将构建机独有的指令集带入镜像；首次编译耗时较长，后续构建可复用缓存。

默认启动执行现有的 `test/test_lcore_layout.py`：自动选择两个可用 CPU，使用
`net_null` 虚拟网卡、256 MiB 普通内存和一秒 DNS 剧本，检查程序退出状态及最终
CSV 记录，成功输出 `PASS` 后退出。它不需要巨页、特权或物理网卡；虚拟网卡没有
真实 DNS 对端，因此这只验证启动和收尾流程，不代表真实请求成功或性能测试通过。
可用 CPU 中编号小于 128 的不足两个时，脚本输出 `SKIP`，不算完成启动验证。

```bash
# 在已构建的镜像中重跑全部单测
docker run --rm --network none snowtg make -C test

# 查看 EAL 参数；也可以用同样方式启动 stack-demo
docker run --rm snowtg traffic-gen --help
```

真实发流需要 Linux 宿主机先准备巨页、启用 IOMMU，并使用
`bind-dpdk.sh` 将**专用压测网卡**绑定到 `vfio-pci`。Docker 不负责配置宿主机
内核或解绑网卡，详见 [DPDK 容器运行说明](https://doc.dpdk.org/guides/linux_gsg/build_sample_apps.html#running-an-application-in-a-container)。
下面示例假定巨页挂载在 `/dev/hugepages`，PCI 地址为 `0000:64:00.0`，
对应的 IOMMU group 为 `17`；运行前替换这些值、CPU 编号、本地 IP，并修改剧本的
`peer.ip` / `peer.port`。可用
`readlink /sys/bus/pci/devices/0000:64:00.0/iommu_group` 查询 group。

```bash
mkdir -p docker-results
docker run --rm --init --network none \
  --device /dev/vfio/vfio --device /dev/vfio/17 \
  --cap-add IPC_LOCK --ulimit memlock=-1:-1 \
  --mount type=bind,src=/dev/hugepages,dst=/dev/hugepages \
  --mount "type=bind,src=$(pwd)/traffic-gen/scenarios,dst=/scenarios,readonly" \
  --mount "type=bind,src=$(pwd)/docker-results,dst=/results" \
  snowtg traffic-gen -l 0-1 -a 0000:64:00.0 \
  --huge-dir /dev/hugepages --file-prefix snowtg -- \
  --workers 1 --local-ip 192.168.21.2 --port-id 0 \
  --stats-csv /results/stats.csv /scenarios/test/mix-http-dns.json
```

这里的流量直接走 VFIO 网卡，不经过 Docker 端口映射。结果保存在宿主机
`docker-results/stats.csv`；多实例应使用不同的网卡、CPU 和 `--file-prefix`。
如果使用 UIO 驱动，需按设备另行配置透传，以上命令仅覆盖 VFIO。

### 运行示例协议栈

按需先将专用压测网卡绑定到 DPDK 驱动，再启动示例程序。脚本支持网卡名或 PCI 地址，默认驱动为 `vfio-pci`：

```bash
./bind-dpdk.sh --status
./bind-dpdk.sh --dry-run enp100s0          # 仅预览，不改变网卡
./bind-dpdk.sh enp100s0                    # NUC：VFIO，需要可用的 IOMMU
# 没有可用 VFIO 的实验虚拟机：显式选择 UIO
./bind-dpdk.sh --driver uio_pci_generic ens160
# 也支持 --driver igb_uio，需事先安装对应模块

./apps/stack-demo/build/stack-demo -l 0-2 ...
```

以登录用户运行脚本，绑定时自动调用 `sudo`；无参数只显示状态。DPDK 工具从
`DPDK_DEVBIND`、`PATH`、`DPDK_DIR/usertools` 或项目相邻的 `../dpdk/usertools`
查找，也可用 `--devbind /path/to/dpdk-devbind.py` 指定。UIO 需要网卡/驱动支持，
不提供 VFIO 的 IOMMU 隔离；脚本不会在 VFIO 失败后自动切换驱动。

脚本默认拒绝解绑有地址或路由的网卡；确认它是专用压测口后可加 `--force`。
当前 SSH 回程使用的网卡仍会被拒绝，需通过独立管理网卡或本地控制台操作。
恢复内核驱动时使用 PCI 地址，例如 NUC 的
`./bind-dpdk.sh --driver igc 0000:64:00.0`，或本 VM 的
`./bind-dpdk.sh --driver vmxnet3 0000:03:00.0`；地址、路由和网卡设置需另行恢复。

TCP/UDP echo 示例及本地地址等编译期开关位于 [`pro-stack/config.h`](pro-stack/config.h)。常用开关包括 `ENABLE_TCP_APP`、`ENABLE_TCP_CLIENT`、`ENABLE_TCP_SERVER`、`ENABLE_UDP_APP`、`ENABLE_ARP` 和 `ENABLE_ICMP`。

### 运行 traffic-gen

DPDK EAL 参数写在 `--` 之前，traffic-gen 参数与 scenario 路径写在 `--` 之后：

```bash
./traffic-gen/build/traffic-gen -l 0-1 -- \
  --workers 1 \
  --local-ip 192.168.21.2 \
  --port-id 0 \
  --stats-csv traffic-gen/results.csv \
  traffic-gen/scenarios/test/mix-http-dns.json
```

完整应用参数为：

```text
traffic-gen [EAL 参数] -- [--workers N] [--socket-id-max N]
            [--stats-csv PATH] [--latency-csv PATH] [--mtu BYTES]
            [--dataplane-csv PATH] [--metrics-sample N]
            [--rx-mode main|worker|auto] [--tx-mode main|worker|auto]
            [--local-ip IPv4] [--port-id N] [scenario.json]
```

- `--workers`：协议栈 owner/reactor worker 数，默认为 `1`。
- `--socket-id-max`：每个 owner 的 socket 容量；省略时启动计算 `max(16384, 2 × ceil(全局并发 / active_shards))`。
  可显式降低默认值，但须至少为 `max(4096, 2 × ceil(全局并发 / active_shards))`；运行中不扩容。
- `--stats-csv`：将周期统计写入指定 CSV 文件。
- `--latency-csv`：按阶段、协议和类别输出延迟直方图及 P50/P90/P95/P99/P99.9，涵盖调度、建连、首字节、完成和排空。
- `--dataplane-csv`：输出 Main 阶段计时、轮询/收发计数和 NIC 统计增量；与 worker CSV 使用不同文件。
- `--metrics-sample`：每 N 轮采样一次新增阶段计时，默认 `1024`；`0` 只保留计数，未指定数据面 CSV 时关闭新增计时。周期字段只覆盖采样轮次，不能直接除以全量包数。
- `--rx-mode`：默认 `auto`；RSS 配置成功且 RX queues 足够时由 worker 独占收包，否则由 Main 软件分流；单 worker 可直接 RX。显式 `worker` 在条件不足时报错，`main` 用于集中 RX 对照。错队列报文经 MP/SC ring 回到 socket owner；ARP 回复和分片重组由 worker 0 负责。
- `--tx-mode`：默认 `auto`；TX queues 足够时每个 worker 独占一个队列，否则回退 Main TX；显式 `worker` 在队列不足时报错。worker 每轮最多发送 4 个 burst，退出前排空。
- `--stats-csv` 新增逐 worker 的 `nic_rx_packets/rx_burst_calls/rx_empty_bursts/rx_full_bursts/rx_handoffs/rx_handoff_drops`。RSS 配置成功不保证虚拟网卡后端实际分流；本机 vmxnet3 实测全部进入 RXQ0，当前推荐显式 `--rx-mode main --tx-mode worker`。FDIR 也需要驱动/硬件支持，本机不支持；证据和 RPS 对照见 [性能记录](docs/PERFORMANCE.md)。
- `--mtu`：设置 IPv4 MTU。
- `--local-ip`：设置协议栈的本机 IPv4 地址，默认为 `192.168.21.2`。
- `--port-id`：选择 EAL 枚举出的 DPDK Ethernet port id，默认为 `0`。
- `scenario.json`：压测剧本；示例位于 [`traffic-gen/scenarios/`](traffic-gen/scenarios/)。

`--local-ip` 和 `--port-id` 配置的是流量发生器本端。scenario 中每个 class
的 `peer.ip` 和 `peer.port` 仍用于配置目标服务地址和服务端口。

### 脚本剧本、SLO 验收与报告

统一入口 `python3 traffic-gen/snowtg.py` 支持 `.json`、`.py`、`.lua`。需要 Python 3.8+；
Lua 剧本另需 Lua 5.3/5.4（可用 `SNOWTG_LUA` 指定解释器）。脚本仅在启动时生成配置，不参与收发包。
直接修改 [Python 示例](traffic-gen/scenarios/test/acceptance-http-dns.py) 或
[Lua 示例](traffic-gen/scenarios/test/acceptance-http-dns.lua) 即可：

- `scenario` 配置全局并发，`http` / `dns` 定义带权重的流量类别。
- `phase` 组合预热、爬坡、稳态、突发和降载；提供 `start` 时线性变速。当前支持开放到达模型。
- `assertion` 设置成功率、延迟分位数、分配失败数和排空等 SLO，可按阶段、类别筛选。
  `success_rate` 分母为计划请求数；`latency_ms` 默认统计成功事务的完成延迟。

以下在项目根目录运行，以 NUC 为例；按机器替换 CPU、PCI、本机 IP。
示例目标为 `192.168.10.234:8888/1053`，可通过 `SNOWTG_PEER` 修改 IP，
用 `SNOWTG_SERVICE_VERSION` 填写实测服务版本。

```bash
python3 traffic-gen/snowtg.py run --output debug/run1 \
  traffic-gen/scenarios/test/acceptance-http-dns.lua -- \
  -l 4,0,2 --main-lcore 4 -a 0000:64:00.0 -m 512 -- \
  --workers 2 --local-ip 192.168.10.86

# 将上面的输出目录换成 debug/run2 再测，然后比较
python3 traffic-gen/snowtg.py compare debug/run1/result.json debug/run2/result.json

# 离线生成含基线差异的报告
python3 traffic-gen/snowtg.py report debug/run2/result.json \
  --baseline debug/run1/result.json --output debug/comparison.html
```

`run` 自动保存 CSV 和日志；`result.json` 记录配置、环境/构建信息与验收结果，`report.html` 展示报告。
输出目录必须是新目录，无需另传 CSV 路径。返回码：`0` 验收通过、`2` 关键 SLO 未达标、`1` 运行无效。
运行时也可在剧本路径前加 `--baseline PATH`；仅导出配置用 `--emit-json PATH`（不带 `run`）。
比较会检查负载及环境是否可比；最大可持续负载仍标为未测定。`debug/` 下的结果不进入 Git。

### 添加应用层协议插件

当前插件机制是源码接入和编译期静态注册，不会在运行时加载 `.so`。一个应用层插件由
两部分组成：[`tg_proto_ops`](traffic-gen/proto/proto.h) 处理请求/响应字节，
[`tg_proto_scenario`](traffic-gen/proto/registry.h) 把 scenario 中的协议对象编译成
不可变配置。HTTP 和 DNS 实现分别位于
[`traffic-gen/proto/http/`](traffic-gen/proto/http/) 和
[`traffic-gen/proto/dns/`](traffic-gen/proto/dns/)，可以直接作为模板。

以新增协议 `myproto` 为例，建议创建以下文件：

```text
traffic-gen/proto/myproto/
├── myproto_client.c
├── myproto_client.h
├── myproto_scenario.c
└── myproto_scenario.h
```

#### 1. 实现字节协议接口

在 `myproto_client.h` 中定义 class 的不可变配置，并导出操作表：

```c
#include "../proto.h"

struct tg_myproto_config {
        /* 必须拥有自身数据，不能引用 scenario JSON 的临时缓冲区。 */
        char request_value[128];
};

extern const struct tg_proto_ops tg_myproto_ops;
```

在 `myproto_client.c` 中实现回调并初始化操作表：

```c
#include "myproto_client.h"
#include "../../core/txn.h"

static int my_config_clone(const void *source, void **destination);
static void my_config_free(void *config);
static int my_init(struct tg_txn *txn);
static int my_build_request(const void *config, uint8_t *buffer,
                            size_t capacity, size_t *length_out);
static enum tg_proto_result my_on_rx(struct tg_txn *txn,
                                     const uint8_t *data, size_t length);
static enum tg_proto_result my_on_eof(struct tg_txn *txn);
static void my_reset(struct tg_txn *txn);

const struct tg_proto_ops tg_myproto_ops = {
    .name = "myproto",
    .config_clone = my_config_clone,
    .config_free = my_config_free,
    .init = my_init,
    .build_request = my_build_request,
    .on_rx = my_on_rx,
    .on_eof = my_on_eof,
    .reset = my_reset,
};
```

实现时遵守以下约定：

- `class_config` 在运行热路径上只读；只要配置非 `NULL`，就必须同时实现
  `config_clone` 和 `config_free`，用于多 worker 分片复制和销毁。
- `init` 可为每个事务分配解析状态并写入 `txn->proto_ctx`；`reset` 必须释放该状态。
- `build_request` 将单次请求序列化到调用方缓冲区，检查容量并填写实际长度。
- TCP 的 `on_rx` 必须支持任意拆分和合并的字节块；UDP 的 `on_rx` 每次收到一个完整
  数据报。插件只解析应用数据，不直接调用 `owner_io_*` 或管理 socket。
- `on_rx`/`on_eof` 返回 `TG_PROTO_MORE`、`TG_PROTO_COMPLETE` 或
  `TG_PROTO_FAILED`。只有响应完整且允许复用 TCP 连接时，才把
  `txn->connection_reusable` 设为 `true`。
- 如需跟踪已被传输层接受的请求字节，可额外实现可选的 `on_tx_accepted`。

#### 2. 编译 scenario 协议对象

实现与 `tg_http_scenario_compile()` 类似的 compiler。它应当：

1. 使用 [`scenario_json.h`](traffic-gen/core/scenario_json.h) 的辅助函数校验
   `myproto` 对象，只接受已声明字段。
2. 分配并完整复制 `tg_myproto_config`，不能保留 JSON token 或文本指针。
3. 设置 `class_plan->proto` 和 `class_plan->proto_config`。
4. 调用 `build_request`，把请求预编译到 `class_plan->request_template`，并设置
   `request_template_len`；请求不能超过 `TG_PLAN_REQUEST_TEMPLATE_CAP`。
5. 任一步骤失败时释放已分配配置、清空所有权字段并设置合适的 `errno`。

然后在 [`registry.c`](traffic-gen/proto/registry.c) 中包含新头文件，并向静态表添加：

```c
{
    .schema_key = "myproto",
    .ops = &tg_myproto_ops,
    .transport = TG_TRANSPORT_TCP, /* 或 TG_TRANSPORT_UDP */
    .compile = tg_myproto_scenario_compile,
},
```

一个 class 必须只包含一个已注册协议键，而且 `transport` 必须与注册项一致。

#### 3. 接入构建并使用

把 `myproto_client.c` 和 `myproto_scenario.c` 加入
[`traffic-gen/Makefile`](traffic-gen/Makefile) 的 `SRCS`。scenario 随后可以这样使用：

```json
{
  "name": "myproto-example",
  "duration_sec": 30,
  "max_concurrency": 100,
  "target_cps": 20,
  "classes": [
    {
      "name": "my-request",
      "weight": 1,
      "transport": "tcp",
      "peer": { "ip": "192.168.21.106", "port": 9000 },
      "myproto": { "request_value": "hello" }
    }
  ]
}
```

建议为插件单独添加协议单测，覆盖请求构造、分片响应、非法响应、EOF 和 reset；同时在
scenario 测试中覆盖合法配置、未知字段、错误 transport 以及配置 clone/free。完成后运行：

```bash
make -C traffic-gen
make -C test
```

### 日志排查

默认构建只保留 TCP/ARP 的警告和错误，避免高 CPS 场景产生大量逐包日志。切换日志编译变量前应先清理旧产物：

```bash
make -C pro-stack clean

# TCP 生命周期与重传日志，不输出逐包日志
make -C pro-stack LOG_LEVEL=LOG_LVL_DEBUG \
  TCP_LOG_INFO_ENABLED=1 TCP_LOG_PACKETS=0

# ARP 调试日志
make -C pro-stack LOG_LEVEL=LOG_LVL_DEBUG ARP_LOG_ENABLED=1

# TCP 逐包日志
make -C pro-stack LOG_LEVEL=LOG_LVL_TRACE \
  TCP_LOG_INFO_ENABLED=1 TCP_LOG_DEBUG_ENABLED=1 \
  TCP_LOG_TRACE_ENABLED=1 TCP_LOG_PACKETS=1
```

使用 `NO_COLOR=1` 或 `LOG_COLOR=never` 可关闭日志颜色。更完整的故障排查说明见 [`docs/DEBUG.md`](docs/DEBUG.md) 和 [`docs/ERROR.md`](docs/ERROR.md)。
