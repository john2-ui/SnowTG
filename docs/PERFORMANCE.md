# traffic-gen 性能记录

短连接与 HTTP keep-alive GET 实测归档。先看「指标说明」，再按日期读结果表；表内只放数字与结论关键词，解释性文字放表下备注。

---

## 1. 测试环境

| 项 | 值 |
| --- | --- |
| 服务端 | `192.168.21.106:8888` |
| 负载形态 | 短连接 / HTTP keep-alive GET（按章节说明） |
| 默认时长 | 120 秒（表内另有说明除外） |
| 客户端 | 本仓库 `traffic-gen` + `pro-stack`，真实 NIC |

目标 CPS 是剧本设定值，不等于实测吞吐；应以 **成功 RPS / 实际 started CPS** 为准。

---

## 2. 指标说明

### 2.1 事务与吞吐

| 指标 | 含义 |
| --- | --- |
| **目标 CPS** | 剧本期望每秒发起的新事务数；调度上限，不是保证值 |
| **最大并发** | 剧本全局 in-flight 上限（`max_concurrency`）；启动时按 active shard **切开** |
| **workers** | 参与发包/状态机的 owner worker（lcore）数；active shard 数为 `min(workers, target_cps, max_concurrency)` |
| **started** | 已发起事务总数（含随后失败的） |
| **done** | 已结束事务总数（成功 + 失败） |
| **success** | L7 成功事务数（HTTP 2xx 等） |
| **fail** | 失败事务数 |
| **成功率** | `success / done × 100%` |
| **实际 started CPS** | `started / 时长`，真实发起速率 |
| **成功 RPS** | `success / 时长`，端到端成功完成速率（主吞吐指标） |

### 2.2 延迟（phase latency）

单位多为毫秒或秒；多 worker 场景记为各 worker 的 **min–max 范围**。

| 指标 | 含义 |
| --- | --- |
| **connect** | 事务开始 → TCP 进入 CONNECTED |
| **first-rx** | 事务开始 → 读到首个应用层响应字节 |
| **complete** | 事务开始 → flow 终结回调（含关闭） |

`avg` / `max` 分别为样本均值与最大值。

### 2.3 资源与丢包

| 指标 | 含义 |
| --- | --- |
| **live_sockets** | 仍占用 socket 槽位的连接数（含关闭中 / TIME_WAIT 等） |
| **live_sockets（结束→排空后）** | duration 结束时 → 排空阶段结束后的 live 数 |
| **tx_peak / payload_peak** | 运行期 TX mbuf / payload 缓冲占用峰值 |
| **paused / pauses** | 因本地资源不足触发的 scheduler 暂停次数相关计数 |
| **tx_alloc_fail** | TX / payload 分配失败次数 |
| **rx_ring_drops** | 软件 RX ring 丢包 |
| **tx_nic_drops** | 发往 NIC 路径上的丢包计数（日志已统计项） |
| **TX / RX** | 累计发送 / 接收包数（aggregate） |
| **ENFILE (errno=23)** | `socket_owner_adopt()` 槽位耗尽（traffic-gen 启动时选定的 per-owner capacity），不是 Linux 进程 fd 限制 |

### 2.4 Dirty TX（2026-08-08 起）

| 指标 | 含义 |
| --- | --- |
| **dirty budget 耗尽** | 单轮 flush 触达 `TX_DIRTY_BUDGET` 的次数 |
| **dirty depth** | dirty FIFO 队列深度相关观测 |
| **arp_wait** | 因 ARP 未解析而挂起等待的 TX 工作 |

路径健康时常见 `budget=0`、`depth=0`、`arp_wait=0`。

### 2.5 失败语义（解读用）

| 日志 / 现象 | 含义与边界 |
| --- | --- |
| **tcp accepted RST** | 本端收到对端 RST；不能单凭客户端日志断定为「对端性能不足」 |
| **rto-give-up kind=syn** | SYN 重传耗尽，connect 以 `ETIMEDOUT` 结束；可能是对端、网络或本端收发路径问题 |
| **资源计数全 0** | 仅排除**已统计**的本端 ring/分配失败；不排除 NIC 硬件计数、链路丢包、未覆盖路径 |

---

## 3. 结果年表

### 3.1 2026-08-05 — 基线与 owner-local FIFO

| 日期 | 改动 | 目标 CPS | 并发 | started | success | fail | 成功率 | 成功 RPS | 相对上次 | 瓶颈 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 08-05 | 基线 | 100 | 1,000 | 11,999 | 11,999 | 0 | 100.00% | — | — | 低负载 |
| 08-05 | 优化前 | 1,000 | 100 | — | — | 44,143 | 63.09% | 628.99 | 未达目标 CPS | per-socket ring、memzone |
| 08-05 | owner-local FIFO | 1,000 | 100 | 106,601 | 106,601 | 0 | 100.00% | 888.34 | 成功 +41.1%；成功 RPS +259（+41%） | 并发 100、短连接 RTT |
| 08-05 | — | 100,000 | 100 | — | — | — | — | — | OOM killer 杀进程 | 每连接分配 sndbuf |

> 基线行未单独记成功 RPS；优化前行以当时日志的 success RPS / 成功率为准。

---

### 3.2 2026-08-06 — ARC-003 复测（含资源峰值）

ARC-003：按需内存分配、去除重复解析 scenario（详见 `DEVLOG.md`）。在真实 NIC 上跑通；**不得**把吞吐仍卡在 ~1k CPS 单纯归因于 mempool。

| 日期 | 改动 | 目标 CPS | 并发 | started | success | fail | 成功率 | 成功 RPS | live（结束→排空） | tx / payload peak | paused | tx_alloc_fail | 相对上次 | 瓶颈 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: | ---: | --- | --- |
| 08-06 | ARC-003 | 10,000 | 100 | 122,934 | 122,934 | 0 | 100.00% | 1,024.45 | 1,899 → 103 | 95 / 99 | 0 | 0 | vs 08-05 FIFO：成功 RPS +15% | 并发 100、RTT；目标 CPS 未触及 |
| 08-06 | ARC-003 | 100,000 | 100 | 115,366 | 115,366 | 0 | 100.00% | 961.38 | 1,937 → 92 | 50 / 50 | 0 | 0 | vs 同日 10k CPS：成功 RPS −6% | 抬目标 CPS 无收益 |
| 08-06 | ARC-003 | 100,000 | 1,000 | 124,291 | 123,422 | 869 | 99.30% | 1,028.52 | ~38 → 2（缺结束快照） | 866 / 868 | 0 | 0 | vs conc=100：吞吐几乎持平 | 对端 RST；并发抬升未转 CPS |
| 08-06 | ARC-003 | 100,000 | 10,000 | — | — | — | — | — | 未正常排空 | — | — | — | 恶化为 socket 表耗尽 | `NSOCK_ID_MAX=4096`；ENFILE |
| 08-07 | ARP 热路径 | 100,000 | 1,000 | 101,694 | 100,847 | 843 | 99.17% | 840.39 | 4（末次采样） | 866 / 868 | 0 | 0 | 无同环境对照，不归因吞吐 | 目标 CPS 未触及；ESTABLISHED RST |

**ARP 热路径（08-07）要点**

- request/reply → `arp_table_learn()`；TCP/UDP 入站 → `arp_table_confirm()`。
- 已存在且 MAC 未变的邻居：只刷新活跃时间，避免重复写缓存。
- `ARP_LOG_ENABLED` 默认关闭，避免热路径日志干扰统计。

---

### 3.3 2026-08-07 — 多 worker

默认：目标 100,000 CPS，时长 120 秒。数字为进程退出时的 aggregate。

#### 吞吐与结果

| workers | 并发 | started | done | success | fail | 成功率 | started CPS | 成功 RPS | TX / RX |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 2 | 1,000 | 74,032 | 74,032 | 72,977 | 1,055 | 98.57% | 616.93 | 608.14 | 2,730,563 / 6,421,976 |
| 4 | 1,000 | 82,449 | 82,449 | 81,525 | 924 | 98.88% | 687.08 | 679.38 | 3,048,874 / 7,174,200 |
| 4 | 10,000 | 78,668 | 78,668 | 66,251 | 12,417 | 84.22% | 655.57 | 552.09 | 2,801,307 / 5,830,088 |

#### 资源、延迟与结论

| workers | 并发 | 资源与丢包 | 延迟（worker 范围） | 结论 |
| ---: | ---: | --- | --- | --- |
| 2 | 1,000 | paused / tx_alloc_fail / rx_ring_drops / tx_nic_drops 均为 0；每 worker tx/payload peak 411–441 | connect avg 66–93 ms；first-rx avg 368–394 ms；complete avg 1.4–2.0 s；complete max 127 s | 远低于目标；1.43% 失败（ESTABLISHED RST、SYN RTO）；worker 启动 43,579 / 30,453，不均衡 |
| 4 | 1,000 | 同上均为 0；peak 219–246 | connect avg 74–106 ms；first-rx avg 345–435 ms；complete avg 1.2–2.9 s；complete max 127 s | vs 2w：成功 RPS +12%；失败率 1.12%；启动 25,473 / 21,394 / 24,993 / 10,589，仍不均衡 |
| 4 | 10,000 | 同上均为 0；peak 2,419–2,451 | connect avg 450–695 ms；first-rx avg 1.1–1.9 s；complete avg 15–18 s；complete max 140 s | vs 4w/1k：成功 RPS −19%；失败率 15.8%；`tokens≈2500` 积压；RST 与完成延迟显著恶化 |

**解读**

- 实际启动速率均不足目标的 0.7%；10k 并发未提升吞吐，反而放大 RST、完成延迟与失败率。
- 主瓶颈不在已统计的 RX ring、NIC 丢包或 TX/payload 分配失败。
- 1k 并发时 4 workers 名义上每核 25k CPS / 250 并发，但 started 仍不均衡；10k 时每核约 2,500 并发，`tokens` 仍积压而 complete 恶化到 15–18 s，说明准入之后完成/关闭路径或对端已饱和。
- 建议对端采 accept 队列、RST/重传、CPU；客户端采 NIC `xstats` 与 SYN/SYN-ACK/RST 抓包，按五元组关联后再归因。

---

### 3.4 2026-08-08 — dirty TX queue

**改动摘要**：worker 全量 `g_sock_list` TX 扫描 → owner-local 去重 dirty FIFO；TCP/UDP 新数据、控制段、RTO、窗口恢复标记 socket；ARP 未解析进入按 IPv4 分桶等待队列；单轮 flush 上限 `TX_DIRTY_BUDGET=64`。

实施前基线（ARC-004）：约 1.6 万 worker turns/s，约 4600–4900 万次 socket scan/flush，`flush_us≈970000`。

#### 复测（4 workers，目标 100k CPS，120 s）

1k 并发行：排空阶段 per-worker `stats` 汇总（日志交错，未见独立 aggregate 行）。  
10k 并发行：中途 `Ctrl+C`（exit 130），无完整排空。

##### 吞吐与结果

| workers | 并发 | started | done | success | fail | 成功率 | started CPS | 成功 RPS | TX / RX |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 4 | 1,000 | 315,774 | 315,724 | 314,112 | 1,612 | 99.49% | 2,631.45 | 2,617.60 | 11,628,138 / 43,347,456 |
| 4 | 10,000 | — | — | — | — | — | — | — | —（未跑满） |

##### 资源、延迟与结论

| workers | 并发 | 资源与丢包 | 延迟（worker 范围） | 结论 |
| ---: | ---: | --- | --- | --- |
| 4 | 1,000 | paused / tx_alloc_fail / ring drops 均为 0；tx/payload peak 157–207；dirty budget/depth/arp_wait=0 | connect avg 45 ms–1.56 s（max ≈30 s）；first-rx avg 107 ms–2.81 s；complete avg 150 ms–4.88 s（max ≈132–140 s） | vs 08-07 同场景成功 RPS 679：+1,938（+285%）；失败率仍约 0.5%；worker 启动 194k / 101k / 14k / 7k，不均衡加剧；dirty TX 健康，全表扫描已非主瓶颈 |
| 4 | 10,000 | 大量 `start failed … errno=23`（ENFILE）；paused / tx_alloc_fail 未见异常 | — | **动态容量改动前的历史结果**：全局 10k 切开后每 shard `active`≈2,500；固定 owner 容量 4,096，`live_sockets`（含 TIME_WAIT）可顶满该表；提高容量可消 ENFILE，不能单独解决 CPS/RST/不均衡 |

> 1k 并发下 dirty TX 显著提升吞吐，但仍远低于 100k CPS。主矛盾转向 worker 负载不均、SYN 失败/超时，以及调度只按 `active` 限流、未计入关闭中 socket。10k 并发仍先撞 ENFILE，无法与 1k 公平对比吞吐。

#### 多 worker 扩展（`http-100000cps-10000con.json`）

命令：`--workers N`；时长 120 s。剧本文件名含 `10000con`；下表 `目标 CPS` / 全局并发以当时 JSON 为准（均按 shard 切开，**不是** `workers × 并发`）。未特别注明时，数字为进程退出时的 aggregate。

##### 吞吐与结果

| 日期 | workers | 目标 CPS | 全局并发 | 每 shard 并发 | started | done | success | fail | 成功率 | started CPS | 成功 RPS | TX / RX |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 08-08 | 8 | 100,000 | 500 | ≈62 | 1,430,504 | 1,430,504 | 1,430,479 | 25 | 99.998% | 11,920.87 | 11,920.66 | 52,927,945 / 197,406,102 |
| 08-12 | 8 | 100,000 | 1,000 | 125 | 499,007 | 499,007 | 498,583 | 424 | 99.915% | 4,158.39 | 4,154.86 | 18,455,378 / 68,804,454 |
| 08-12 | 4 | 100,000 | 500 | 125 | 577,585 | 577,585 | 577,559 | 26 | 99.995% | 4,813.21 | 4,812.99 | 21,370,090 / 79,703,142 |
| 08-12 | 8 | 15,000 | 500 | ≈62 | 505,996 | 505,996 | 505,946 | 50 | 99.990% | 4,216.63 | 4,216.22 | 18,720,742 / 69,820,548 |
| 08-12† | 8 | 100,000 | 5,000 | 625 | 473,828 | 473,828 | 468,228 | 5,600 | 98.818% | 3,948.57 | 3,901.90 | 17,420,192 / 64,615,464 |

† 排空阶段 `Ctrl+C`，无 `aggregate` 行；表内数字由 8 个 worker 末次稳定 `stats` 求和，CPS/RPS 仍按 `duration_sec=120` 计算。

##### 资源、延迟与结论

| 日期 | workers | 目标 CPS | 全局并发 | 资源与丢包 | 延迟（worker 范围） | 结论 |
| --- | ---: | ---: | ---: | --- | --- | --- |
| 08-08 | 8 | 100,000 | 500 | paused / tx_alloc_fail / rx_ring_drops / tx_nic_drops 均为 0；每 worker tx/payload peak ≈62–63；dirty budget/depth/arp_wait=0（运行中 dirty_hwm 约 15–50）；固定 owner 容量 4,096 | connect avg ≈14–16 ms（max ≈2–4 s）；first-rx avg ≈41–43 ms；complete avg ≈41–43 ms（max ≈32 s） | vs 同日 4w/1k：成功 RPS +9,303（+355%）；失败仅 25（多为 `rto-give-up kind=syn`）；worker started 172k–184k（比值 1.07） |
| 08-12 | 8 | 100,000 | 1,000 | paused / tx_alloc_fail / rx_ring_drops / tx_nic_drops 均为 0；每 worker tx/payload peak ≈110–125；dirty budget/depth/arp_wait=0（dirty_hwm 0–1）；自动 `socket_id_capacity=4096`（`max(4096, 2×125)`），无 ENFILE | connect avg ≈138–147 ms（max ≈8.1–8.2 s）；first-rx avg ≈223–237 ms（max ≈8.1–17.1 s）；complete avg ≈232–250 ms（max ≈32.1–32.4 s） | vs 08-08 8w/500：成功 RPS **−7,766（−65%）**；失败 424（排空期大量 `rto-give-up kind=syn`）；worker started 61.2k–65.1k（比值 1.06） |
| 08-12 | 4 | 100,000 | 500 | paused / tx_alloc_fail / rx_ring_drops / tx_nic_drops 均为 0；每 worker tx/payload peak ≈115–123；dirty budget/depth/arp_wait=0（运行中 dirty_hwm 约 15–64）；`socket_id_capacity=4096`，无 ENFILE；稳态 `active=125`、`tokens≈125` | connect avg ≈57–58 ms（max ≈2.1–4.1 s）；first-rx avg ≈102–105 ms（max ≈4.1–7.1 s）；complete avg ≈103–105 ms（max ≈7.1–32.0 s） | vs 08-08 8w/500：成功 RPS **−7,108（−60%）**；vs 同日 8w/1k：成功 RPS +658（+16%）；失败仅 26；worker started 143.3k–146.2k（比值 1.02） |
| 08-12 | 8 | 15,000 | 500 | paused / tx_alloc_fail / rx_ring_drops / tx_nic_drops 均为 0；每 worker tx/payload peak ≈49–62；dirty budget=0、depth≈11–17、hwm≈21–28；`socket_id_capacity=4096`，无 ENFILE；稳态 `tokens≈60–63`（贴每 shard ≈62） | connect avg ≈63–66 ms（max ≈2.1–4.1 s）；first-rx avg ≈116–120 ms（max ≈3.3–7.1 s）；complete avg ≈116–124 ms（max ≈32.0–32.3 s） | vs 08-08 **同结构** 8w/500：成功 RPS **−7,704（−65%）**；vs 同日 8w/1k：RPS 基本持平（+61）；未打到 15k 目标（仅用到约 28%）；失败 50；worker started 60.3k–65.3k（比值 1.08） |
| 08-12† | 8 | 100,000 | 5,000 | paused / tx_alloc_fail / rx_ring_drops / tx_nic_drops 均为 0；每 worker tx/payload peak ≈310–356；`socket_id_capacity=4096`（`max(4096, 2×625)`），无 ENFILE / 未见 `local port allocation failed`；稳态 `tokens=625`；排空末仍有 live 5–30 | connect avg ≈617–685 ms（max ≈16–17 s）；first-rx avg ≈1.06–1.15 s（max ≈35–67 s）；complete avg ≈1.25–1.42 s（max ≈63–125 s） | vs 同日 8w/500/15k：成功 RPS **−314（−7%）**；complete 从 ~120 ms 恶化到 **~1.3 s**；失败 5,600（成功率降至 98.8%）；worker started 54.4k–62.4k（比值 1.15） |

**解读**

- **08-08 / 8w / 500 / 100k CPS**：实际 started CPS ≈ 11.9k。Little's law：每 worker ≈1.5k CPS × 41 ms ≈ 61 in-flight，打满每 shard ≈62；资源侧干净。
- **08-12 / 8w / 1k / 100k CPS**：实际 ≈ 4.16k；每 shard 125 + complete ~244 ms → in-flight 贴满配额；抬并发后延迟恶化，吞吐下降。
- **08-12 / 4w / 500 / 100k CPS**：实际 ≈ 4.81k；每 shard 125 打满，complete ~104 ms；略优于同日 8w/1k，仍远低于 08-08。
- **08-12 / 8w / 500 / 15k CPS**：实际 ≈ 4.22k（目标的 28%）。与 08-08 **同为 8w + 全局 500**，但 complete 从 ~42 ms 恶化到 ~118 ms；Little's law：\(500 / 0.118 \approx 4.2\mathrm{k}\)，与实测一致。目标 CPS 已收到 15k（高于实测），**不是 token 瓶颈**；降 CPS 目标**未能**回到 08-08 的 ~12k，说明当日环境/对端延迟底噪已差于 08-08，或存在未对照的本端回归。
- **08-12 / 8w / 5k / 100k CPS**：实际 ≈ 3.95k。每 shard 625；Little's law：\(5000 / 1.33\mathrm{s} \approx 3.8\mathrm{k}\)，与实测一致。相对同日 500 并发档，并发×10 只换来更差的延迟与更多失败，RPS 不升反降。每 shard 625 仍低于 RSS 切分后的 ephemeral 端口池（约 ~2k/核），故未出现 10k 档的 `local port allocation failed`。
- **08-12 / 8w / 10k（未完整入表）**：稳态 `active=1250`、`live_sockets≈2000+`，大量 `tcp_connect: local port allocation failed` / `start failed errno=11`——端口池先于 socket 表耗尽（详见下文开放项）。
- 低并发档负载均衡尚可（started 比值 ≈1.02–1.08）；5k 档比值升至 1.15。失败以 SYN RTO / 对端 RST 为主。
- **socket 表**：ENFILE 见于高 per-shard `live` 顶满固定 4,096 时（如 4w/10k），不是 `workers × max_concurrency`。08-12 的 500/1k/5k 自动容量下均未撞表。
- **socket 容量策略（当前代码）**：`max(4096, 2 × ceil(max_concurrency / active_shards))`，可用 `--socket-id-max N` 增大。临时端口范围为 `49152–65535`（16384 个），再按 RSS 亲和切到各 worker。

##### 08-12 并发扫参 + SYN 抓包（材料 [`debug/2026-08-12/`](../debug/2026-08-12/)）

固定 8w、`target_cps=8000`，扫全局并发 250/500/750（~120s）。RPS 约 1205 → 1151 → 1138，complete p50 约 42 → 106 → 158 ms；无 ENFILE/端口耗尽。抓包 SYN→SYN-ACK：250 档 p50/p99 ≈19/35 ms、几乎无重传；750 档 p50/p99 ≈29/**1057** ms，SYN retrans 流约 4.5%。

**原因**：吞吐受「并发 ÷ 完成时延」约束；抬并发后对端/路径握手变慢（长尾+重传），占满客户端并发槽，RPS 不升反降——不是本机 TX/socket 表发不动。08-08→08-12 同结构 ~12k→~4k 的底噪差异仍可能含环境/对端变化，未单独做回归对照。

---

### 3.5 2026-08-13 — HTTP keep-alive 与短连接对照

固定 8 workers、目标 100,000 CPS、`duration=120 s`、对端
`192.168.21.106:8888`，只改变 HTTP `keepalive`。短连接 CSV 与
`-keepalive.csv` 使用相同并发档位；成功 RPS 为 `success / 120 s`。
复现实验脚本、统计口径和 Canvas 归档见
[`debug/2026-08-13/`](../debug/2026-08-13/)。

#### 吞吐、可靠性与时延

| 并发 | 模式 | started | success | fail | 成功率 | 成功 RPS | complete avg |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 500 | 短连接 | 477,148 | 474,412 | 2,736 | 99.427% | 3,953.43 | 126.1 ms |
| 500 | keep-alive | 1,730,148 | 1,727,547 | 2,601 | 99.850% | **14,396.23** | **34.6 ms** |
| 1,000 | 短连接 | 457,686 | 456,950 | 736 | 99.839% | 3,807.92 | 263.0 ms |
| 1,000 | keep-alive | 1,685,541 | 1,650,874 | 34,667 | 97.943% | **13,757.28** | **71.3 ms** |
| 5,000 | 短连接† | 517,253 | 463,892 | 53,361 | 89.684% | 3,865.77 | 1,172.1 ms |
| 5,000 | keep-alive† | 1,354,940 | 1,132,166 | 222,774 | 83.558% | **9,434.72** | **445.1 ms** |

† 5,000 档短连接与三份 keep-alive CSV 都没有完整 aggregate/final
行，采用最后一个完整的 8-worker 累计快照；因此不能把它们当作已完成排空的
最终结果。

- keep-alive 的成功 RPS 相对同日短连接提高 **3.64× / 3.61× / 2.44×**，
  complete 平均时延下降 **72.6% / 72.9% / 62.0%**。
- 500 并发 keep-alive 的 14,396 RPS 高于 08-08 的短连接最佳
  11,920.66 RPS（约 **+20.8%**）；跨日期差异仍只能作为参考。
- 与 08-12 的短连接 8w/1k、8w/5k（4,154.86、3,901.90 RPS）相比，
  keep-alive 分别达到约 **3.31×、2.42×**，但高并发可靠性明显下降。

#### 连接池负载与握手压力

短连接三档均为 `connections_reused=0`，基本每个事务都要新建 TCP；
keep-alive 三档均满足不变量
`connections_created + connections_reused == started`：

| 并发 | pool max / shard | created | reused | 复用率 | 平均事务 / 物理连接 | live_sockets：运行末 → 最后快照 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 500 | ≈62/63 | 18,867 | 1,711,281 | 98.91% | 91.7 | 845 → 38 |
| 1,000 | 125 | 40,293 | 1,645,248 | 97.61% | 41.8 | 1,382 → 210 |
| 5,000 | 625 | 225,623 | 1,129,317 | 83.35% | 6.0 | 8,423 → 737 |

keep-alive **不是扩大服务端 `accepted_queue`**，而是把大量逻辑事务复用
到少量物理 TCP 连接上：例如 500 并发用约 18,867 条物理连接承载
1,730,148 个事务。新建连接速率下降后，SYN/SYN-ACK、服务端 accept
和短连接关闭的压力同步下降，这与 accepted queue 不再被频繁握手占满的
机制一致。客户端侧也能看到 `fail_connect` 从短连接的
2,732 / 254 / 36,645 降至 keep-alive 的 0 / 5 / 10,670。

本次 CSV 没有服务端 accept queue 深度或 overflow/drop 计数，因此上面是
由连接复用、建连失败和吞吐变化支持的解释，不是对 queue 的直接测量；后续
仍应同时采集服务端 accept queue、`ListenOverflows/ListenDrops` 及
SYN/RST 抓包。

高并发的代价也很清楚：keep-alive 失败从 `fail_connect` 转移到已建立连接
上的 I/O/HTTP protocol。1,000 档 `fail_proto=22,368`，5,000 档
`fail_proto=175,060`；同时 5,000 档平均每条物理连接只承载约 6 个事务，
连接池进入高 churn，而不是单纯的握手瓶颈。六次运行的
`tx_alloc_fail`、`rx_ring_drops`、`tx_nic_drops` 均为 0。

---

## 4. 当前结论（截至 2026-08-13）

分档数字见 §3；此处只留总览。

| 观察 | 说明 |
| --- | --- |
| 最佳短连接实测 | 08-08：8w / 并发 500 / 目标 100k → 成功 RPS ≈ **11,921** |
| keep-alive 最高实测 | 08-13：8w / 并发 500 / 目标 100k → 成功 RPS ≈ **14,396**；复用率 98.91% |
| 08-12 水位 | 同结构约 **3.9k–4.8k** RPS，远低于 08-08；抬并发 complete 变差、RPS 不升 |
| 主因（并发恶化） | 扫参+抓包：SYN 长尾/重传 → **对端/路径过载**占满并发槽（材料 `debug/2026-08-12/`） |
| keep-alive 收益 | 减少重复握手，显著降低 `fail_connect`，缓解 accepted queue 的握手压力；同日 500/1k/5k 成功 RPS 提升 3.64×/3.61×/2.44× |
| keep-alive 边界 | 1k/5k 失败转向已建立连接的 I/O/protocol；5k 成功率 83.558%，排空后仍有 737 live sockets |
| 本机已排除 | 已统计项内无 mempool / TX·payload alloc / ring·nic drop；中低并发未撞 socket 表 |
| 本机天花板 | 极高并发（~10k）先撞 **临时端口**（RSS 切分），不是 ENFILE |
| 仍开放 | 服务端 accept queue 计数、keep-alive 响应 framing/RST、关闭中 socket / 端口背压 |
| 对比纪律 | 无同环境对照的改动不对吞吐变化做归因 |
