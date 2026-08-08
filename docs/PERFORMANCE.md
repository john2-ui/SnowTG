# traffic-gen 性能记录

短连接 HTTP GET 实测归档。先看「指标说明」，再按日期读结果表；表内只放数字与结论关键词，解释性文字放表下备注。

---

## 1. 测试环境

| 项 | 值 |
| --- | --- |
| 服务端 | `192.168.21.106:8888` |
| 负载形态 | 短连接 HTTP GET |
| 默认时长 | 120 秒（表内另有说明除外） |
| 客户端 | 本仓库 `traffic-gen` + `pro-stack`，真实 NIC |

目标 CPS 是剧本设定值，不等于实测吞吐；应以 **成功 RPS / 实际 started CPS** 为准。

---

## 2. 指标说明

### 2.1 事务与吞吐

| 指标 | 含义 |
| --- | --- |
| **目标 CPS** | 剧本期望每秒发起的新事务数；调度上限，不是保证值 |
| **最大并发** | 允许同时 in-flight 的事务上限（scheduler concurrency） |
| **workers** | 参与发包/状态机的 owner worker（lcore）数 |
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
| **ENFILE (errno=23)** | `socket_owner_adopt()` 槽位耗尽（`NSOCK_ID_MAX`），不是 Linux 进程 fd 限制 |

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
| 4 | 10,000 | 大量 `start failed … errno=23`（ENFILE）；paused / tx_alloc_fail 未见异常 | — | 与历史同场景一致：`NSOCK_ID_MAX=4096`，live（含 TIME_WAIT）高于 per-worker 并发（≈2,500）；调大上限可消 ENFILE，不能单独解决 CPS/RST/不均衡 |

> 1k 并发下 dirty TX 显著提升吞吐，但仍远低于 100k CPS。主矛盾转向 worker 负载不均、SYN 失败/超时，以及调度只按 `active` 限流、未计入关闭中 socket。10k 并发仍先撞 ENFILE，无法与 1k 公平对比吞吐。

#### 8 workers 扩展（`http-100000cps-10000con.json`，`max_concurrency=500`）

命令：`--workers 8`；时长 120 s；目标 100k CPS。剧本文件名含 `10000con`，实际 `max_concurrency=500`，总并发预算 `8 × 500 = 4,000`（贴近 `NSOCK_ID_MAX=4096` 可用上限）。数字为进程退出时的 aggregate。

##### 吞吐与结果

| workers | 并发（每 worker） | 总并发预算 | started | done | success | fail | 成功率 | started CPS | 成功 RPS | TX / RX |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 8 | 500 | 4,000 | 1,430,504 | 1,430,504 | 1,430,479 | 25 | 99.998% | 11,920.87 | 11,920.66 | 52,927,945 / 197,406,102 |

##### 资源、延迟与结论

| workers | 并发 | 资源与丢包 | 延迟（worker 范围） | 结论 |
| ---: | ---: | --- | --- | --- |
| 8 | 500 | paused / tx_alloc_fail / rx_ring_drops / tx_nic_drops 均为 0；每 worker tx/payload peak ≈62–63；dirty budget/depth/arp_wait=0（运行中 dirty_hwm 约 15–50） | connect avg ≈14–16 ms（max ≈2–4 s）；first-rx avg ≈41–43 ms；complete avg ≈41–43 ms（max ≈32 s） | vs 同日 4w/1k：成功 RPS +9,303（+355%）；失败仅 25（多为 `rto-give-up kind=syn`，少量 LAST_ACK RST）；worker started 172k–184k（比值 1.07），负载均衡显著改善 |

**解读**

- 实际 started CPS ≈ 11.9k，约为目标 100k 的 12%；相对 4w/1k 的跃升主要来自更多 worker + 完成延迟压到约 40 ms（Little's law：每 worker ≈1.5k CPS × 41 ms ≈ 61 in-flight，与采样 `active≈62–63` 一致，远未打满 500 并发配额）。
- 资源侧仍干净：无 mempool/ring/NIC 软件丢包计数异常；dirty TX 路径健康。
- 失败几乎全是 connect 超时（SYN RTO），占比可忽略；排空阶段偶发 `rto-give-up kind=syn`。
- **socket 表上限**：当前 `workers × max_concurrency` 实测只能稳在约 **4,000** 附近；再抬高会触发 socket 表空 / `ENFILE`（`NSOCK_ID_MAX=4096`，live 含 TIME_WAIT 等关闭中槽位）。本档 4,000 预算刚好贴边，未再撞 ENFILE，但也说明靠继续堆并发预算换 CPS 的空间已很小，除非先抬高 `NSOCK_ID_MAX` 并解决关闭中 socket 占用。

---

## 4. 当前结论（截至 2026-08-08）

| 观察 | 说明 |
| --- | --- |
| 最佳实测档 | 8 workers × 500 并发（总预算 4k）+ dirty TX：成功 RPS ≈ **11,921**，成功率 ≈ 99.998% |
| 未达目标 | 剧本 100k CPS 仍未触及；实际 started CPS 约 11.9k（约目标的 12%） |
| 已排除（在已统计项内） | mempool 耗尽、TX/payload alloc fail、RX ring / tx_nic drops、dirty 队列堆积 |
| socket 表天花板 | `workers × max_concurrency` 约 **4,000** 为稳跑上限；更高易 socket 表空 / ENFILE（`NSOCK_ID_MAX=4096`） |
| 仍开放 | SYN RTO / 对端 RST、关闭中 socket 与 concurrency 记账、`NSOCK_ID_MAX` 扩容、目标 CPS 剩余约 8× 差距 |
| 对比纪律 | 无同环境对照的改动（如部分 ARP 优化行）不对吞吐变化做归因 |
