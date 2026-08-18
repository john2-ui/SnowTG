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

### 运行示例协议栈

按需先将目标网卡绑定到 DPDK 驱动，再启动示例程序：

```bash
./bind-dpdk.sh
./apps/stack-demo/build/stack-demo -l 0-2 ...
```

TCP/UDP echo 示例及本地地址等编译期开关位于 [`pro-stack/config.h`](pro-stack/config.h)。常用开关包括 `ENABLE_TCP_APP`、`ENABLE_TCP_CLIENT`、`ENABLE_TCP_SERVER`、`ENABLE_UDP_APP`、`ENABLE_ARP` 和 `ENABLE_ICMP`。

### 运行 traffic-gen

DPDK EAL 参数写在 `--` 之前，traffic-gen 参数与 scenario 路径写在 `--` 之后：

```bash
./traffic-gen/build/traffic-gen -l 0-1 -- \
  --workers 1 \
  --stats-csv traffic-gen/results.csv \
  traffic-gen/scenarios/test/mix-http-dns.json
```

完整应用参数为：

```text
traffic-gen [EAL 参数] -- [--workers N] [--socket-id-max N]
            [--stats-csv PATH] [--mtu BYTES] [scenario.json]
```

- `--workers`：协议栈 owner/reactor worker 数，默认为 `1`。
- `--socket-id-max`：手动设置每个 owner 的 socket 容量；省略时根据 scenario 自动计算。
- `--stats-csv`：将周期统计写入指定 CSV 文件。
- `--mtu`：设置 IPv4 MTU。
- `scenario.json`：压测剧本；示例位于 [`traffic-gen/scenarios/`](traffic-gen/scenarios/)。

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
