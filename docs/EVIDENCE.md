# 客户端、网络与服务端证据关联

`snowtg.py run --evidence manifest.json` 在运行结束时归档证据，并将关联写入 `result.json.correlation` 和 HTML。`correlate` 可处理已保存的单次结果；不需要重跑负载。关联是辅助诊断，不改变吞吐、延迟、有效性、SLO 或原始退出码。

## 采集与使用

分别在客户端和服务端本机运行采样器，在负载开始前启动、排空结束后停止。输出文件必须不存在；将两端文件收集到同一证据目录：

```bash
python3 traffic-gen/snowtg.py monitor --interval 1 --samples 600 \
  --process traffic-gen --process nginx --output client-host.jsonl
```

采样器记录主机名、启动 ID、实时时钟、单调时钟、CPU、内存、接口、内核 TCP/IP 计数及所选进程。服务端使用同样命令并更换进程名及输出路径；默认进程名为 `traffic-gen`、`nginx`、`dnsmasq`、`redis-server`。`test/acceptance_monitor.py` 是仓库内兼容入口，单独复制该文件不能替代完整模块。工具不执行 SSH、不注入故障、不调整时钟。

在收集目录保存 `manifest.json`。下面的时钟数字只是格式示例，必须替换为本轮实测值；两台机器不能未经测量就声明零偏差。

```json
{
  "schema_version": 1,
  "reference_host": "client",
  "client_host": "client",
  "window_sec": 5,
  "server_slow_ms": 100,
  "clock_offsets": [
    {"host": "server", "offset_ns": 20000000, "uncertainty_ns": 1000000}
  ],
  "sources": [
    {"kind": "host", "host": "client", "role": "client", "path": "client-host.jsonl", "interfaces": ["eth0"]},
    {"kind": "host", "host": "server", "role": "server", "path": "server-host.jsonl", "interfaces": ["eth0"]},
    {"kind": "requests", "host": "server", "path": "requests.jsonl"},
    {"kind": "faults", "host": "client", "path": "faults.jsonl"}
  ]
}
```

`path` 相对 manifest 目录解析，也可为绝对路径。无需某一来源时删除该项，不用空文件伪造正常状态。`interfaces` 省略时读取全部内核接口；它不能采集已绑定 DPDK 的接口计数，DPDK NIC 指标由原生运行提供。`clock_offsets` 也可以是含相同数组的 JSON 文件路径，兼容已有验收偏差文件。主机标识由 manifest 定义，须与偏差记录一致。

```bash
# 加入现有 run 命令，其余场景、EAL、app 参数保持原有用法
python3 traffic-gen/snowtg.py run --evidence debug/evidence/manifest.json \
  --output debug/correlated-run traffic-gen/scenarios/test/acceptance-http-dns.py -- \
  -l 4,0,2 --main-lcore 4 -a 0000:64:00.0 -m 512 -- \
  --workers 2 --local-ip 192.168.10.86

# 离线附加；输出必须是新目录，不覆盖原结果
python3 traffic-gen/snowtg.py correlate debug/run1/result.json \
  --evidence debug/evidence/manifest.json --output debug/run1-correlated

# 已附加证据的结果可照常重建 HTML
python3 traffic-gen/snowtg.py report debug/run1-correlated/result.json \
  --output debug/run1-correlated.html
```

离线命令成功生成报告返回 `0`，即使证据为 `partial`；命令错误返回 `1`。它不是新的 SLO 门禁。容量搜索应对具体轮次的 `result.json` 使用 `correlate`，不对整个容量搜索结果做时间关联。

## 来源格式

所有来源都是 UTF-8 JSONL，每条记录必须以换行结束。时刻与时长使用整数纳秒。

| kind | 字段及语义 |
| --- | --- |
| `host` | 兼容采样器的 `/proc` 原始记录，必需 `wall_time_ns`、`monotonic_ns`；相邻记录转换为区间差值。新格式另有 `boot_id`、`ticks_per_second`。 |
| `requests` | `time_ns`、`protocol`（`http`/`dns`/`redis`）；无 `duration_ns` 表示到达，有 `duration_ns` 表示处理完成，`time_ns` 为完成时刻。两类记录分别计数。 |
| `faults` | `start_ns`、`end_ns`、`category`（`client`/`network`/`server`），可加 `label`。表示用户声明的故障区间，不能证明故障已生效。 |

请求示例：

```json
{"time_ns":1790570000000000000,"protocol":"http","event":"arrival"}
{"time_ns":1790570000200000000,"protocol":"http","event":"complete","duration_ns":200000000}
```

`test/pressure_peer.py` 现在同时记录 HTTP/DNS 到达与处理完成，HTTP 耗时含延迟和写回等待，DNS 耗时含延迟与发出回包；未回包的 DNS drop 没有完成记录。HTTP 处理结束可包括错误返回，完成记录不表示请求成功。统计对端到达数应筛选 `event=arrival`，不能把所有日志行相加。旧版仅有到达的日志仍可计数，但不能证明服务变慢。

故障示例：

```json
{"start_ns":1790570000000000000,"end_ns":1790570005000000000,"category":"network","label":"测试链路丢包窗口"}
```

兼容已有 controller 的 `fault_state`（HTTP/DNS config）与 `netem`（settings）状态变化日志；下次同类记录关闭前一个区间，`{}` / 正常模式以及空 settings / `delay 0ms` 表示恢复。没有结束记录则不推定结束时刻；其他 controller 事件忽略，任何 `command` 都不会执行。新接入优先使用明确起止时间的规范格式。

## 时钟与报告边界

偏差定义为 **主机实时时钟 − reference_host 实时时钟**。参考主机偏差为零；同一主机多次偏差测量取覆盖所有样本的不确定区间。用户声明偏差适用于本轮，静态偏差无法证明期间没有漂移。

新版原生运行记录首尾 `clock_anchor`，将共享负载 epoch 映射到客户端实时时钟；锚点采样误差、首尾漂移、跨机偏差不确定度一并保留。主机采样显示重启、乱序或时钟跳变时，该主机静态映射失效；客户端或参考主机失效会禁止全部外部同期结论。总误差超过关联窗口一半时也禁止同期结论。请求靠近窗口边界时记录 `boundary_uncertain`；区间可能与多个窗口重叠，不能把重叠窗口计数直接相加。

旧结果没有原生锚点时，不能用 launcher 的 `started_utc` 冒充负载起点。只有确有测量依据时才在 manifest 加入：

```json
"run_start": {"host": "client", "time_ns": 1790570000000000000, "uncertainty_ns": 1000000}
```

它表示该 host 时钟上的实际负载 epoch，不是进程启动或观察到启动的时间；不能覆盖新版已检测出的时钟跳变。缺少该映射的旧数据仍归档，但不输出跨机同期结论。旧 NIC 数据没有独立采样原点也不强行对齐。

报告以 `window_sec` 分窗，列出客户端 worker、owner 资源、NIC、主机区间、请求完成耗时和故障引用。每个引用都是详情数组的零基下标；请求桶引用 source ID。结论允许同时出现：

- **客户端并发保护**：原生 `concurrency_blocked_turns` 增量表示有待发到达且并发已满的调度轮数；不是请求数、阻塞时长或跳过请求数。不能由此推断并发满的上游原因。旧结果未采集该计数时不反推。
- **客户端资源压力**：内存暂停、资源延后，或完整 owner 趋势中的失败计数增量；不同资源层不能相加解释失败请求数。NIC `imissed` / `rx_nombuf` 单列为客户端设备压力。
- **同期网络异常候选**：客户端有失败、跳过或区间平均完成延迟超过 `server_slow_ms`，同时存在网络故障窗口、接口丢包/错误、TCP 重传/超时、NIC 错误。主机级计数可能包含其他流量，内核零计数也不证明 DPDK 链路正常。
- **服务端变慢证据**：对端日志存在处理耗时超过 `server_slow_ms` 的完成记录；只有到达日志、CPU 忙或声明注入 slow 不足以证明实际耗时。声明的服务端故障窗口单列显示。
- **仍不可归因**：客户端退化窗口保留未知贡献，即使已有同期候选。没有共享请求 ID，不进行逐请求因果匹配，也不用客户端延迟减服务端耗时推算网络延迟。

`server_slow_ms` 默认 100，仅是证据提示阈值，不替代场景 SLO。时序延迟是区间均值，不能当作区间 P99。报告状态 `local_only` 表示未提供外部来源，`available` 表示所提供来源可用，`partial` 表示有证据缺口；`available` 不表示根因已确定或观测完整。

原始输入复制到 `evidence/`，保存 SHA-256、初始文件长度和来源；持续增长文件仅截取开始复制时的长度，末尾半行作为截断记录。保存后的 manifest 使用快照相对路径，可再次离线导入。单文件上限 512 MiB、单行 2 MiB、来源 32 个、主机/故障区间分别 20,000 个、关联窗口至多 10,000 个（长运行自动加宽窗口）。请求日志流式聚合；缺失文件、坏行、计数回退和无法对齐的来源都显示缺口。

## 验证

```bash
make -C test test-evidence
make -C test test-evidence-cli
python3 test/test_acceptance_peer.py --pressure
```

默认回归包含证据规则与离线 CLI；双 worker `net_null` 测试覆盖原生时钟、并发保护计数、NIC 与 owner 时间窗、自动归档、JSON/HTML 及 SLO 不变；loopback 对端验证慢响应处理耗时。2026-09-28 已导入保存的双机 HTTP slow 验收日志（含 244,205 条到达记录），正确输出缺少原生时间锚点及处理耗时的 `partial` 报告；没有将历史人工观察升级为自动因果结论。新版采集链路的真实双机复验仍待执行，本地测试不能替代该验收。
