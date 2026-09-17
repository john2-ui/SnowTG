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
  --local-ip 192.168.21.2 \
  --port-id 0 \
  --stats-csv traffic-gen/results.csv \
  traffic-gen/scenarios/test/mix-http-dns.json
```

完整应用参数为：

```text
traffic-gen [EAL 参数] -- [--workers N] [--socket-id-max N]
            [--stats-csv PATH] [--mtu BYTES]
            [--local-ip IPv4] [--port-id N] [scenario.json]
```

- `--workers`：协议栈 owner/reactor worker 数，默认为 `1`。
- `--socket-id-max`：每个 owner 的 socket 容量；省略时启动计算 `max(16384, 2 × ceil(全局并发 / active_shards))`。
  可显式降低默认值，但须至少为 `max(4096, 2 × ceil(全局并发 / active_shards))`；运行中不扩容。
- `--stats-csv`：将周期统计写入指定 CSV 文件。
- `--mtu`：设置 IPv4 MTU。
- `--local-ip`：设置协议栈的本机 IPv4 地址，默认为 `192.168.21.2`。
- `--port-id`：选择 EAL 枚举出的 DPDK Ethernet port id，默认为 `0`。
- `scenario.json`：压测剧本；示例位于 [`traffic-gen/scenarios/`](traffic-gen/scenarios/)。

`--local-ip` 和 `--port-id` 配置的是流量发生器本端。scenario 中每个 class
的 `peer.ip` 和 `peer.port` 仍用于配置目标服务地址和服务端口。

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
