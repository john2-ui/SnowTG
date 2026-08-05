# traffic-gen 性能提升与评测记录

本文档记录 `traffic-gen` 的性能相关事件、当前实现边界、可复现实测结果及后续优化计划。
所有性能结论均以实际运行命令、场景文件和终端统计为准；未经实测的容量不作为能力承诺。

## 1. 当前实现进度

| 项目 | 状态 | 说明 |
|---|---|---|
| TCP/HTTP 短连接压测 | 已实现 | 每个 flow 发起 TCP 连接、发送 HTTP 请求、解析完整 2xx 响应后关闭。 |
| CPS 限速 | 已实现 | scheduler 按 `target_cps` 发起连接尝试。 |
| 并发上限 | 已实现 | scheduler 使用 `max_concurrency` 限制在途 flow；当前 socket owner 容量上限为 4096。 |
| 累积统计 | 已实现 | 输出 started、done、success、fail、应用层 TX/RX 字节数及失败分类。 |
| duration 到期摘要 | 已实现 | 停止发车时打印一次累计摘要和成功率。 |
| 自动结束 | 未实现 | 达到 `duration_sec` 后仅停止发车并继续 drain；主 lcore 仍无限循环，需手工终止。 |
| 延迟统计 | 未实现 | 尚无单事务耗时、P50/P95/P99。 |
| 窗口化速率 | 未实现 | 日志是累计计数；RPS/CPS 需由相邻采样差值计算。 |
| 多 worker / RSS | 未实现 | 当前一个 socket owner worker 承载全部 flow。 |

## 2. 评测指标与口径

| 指标 | 计算方式 | 用途 |
|---|---|---|
| 事务尝试率 | `started / duration_sec` | 验证 scheduler 是否接近目标 CPS。 |
| 成功 RPS | `success / duration_sec` | 端到端完成的 HTTP 短连接事务速率。 |
| 成功率 | `success / done × 100%` | 判断流量是否因客户端、网络、协议或服务端错误而损失。 |
| 失败分类 | `fail_connect`、`fail_io`、`fail_proto` | 定位连接建立、传输 I/O 或 HTTP 解析问题。 |
| 到期在途数 | duration 摘要中的 `active` | 判断停止发车时是否仍有待排空事务。 |
| 应用层吞吐 | `bytes_tx/rx / duration_sec` | 仅统计请求与响应 payload，不代表网卡线速。 |
| 峰值并发 | 后续需新增 | 现有 `active` 只显示采样瞬间，不能代表峰值。 |
| 延迟分位数 | 后续需新增 | 需记录 flow 的起止 cycle 并维护直方图。 |

## 3. 已记录事件

| 时间 | 事件 | 影响与结论 |
|---|---|---|
| 2026-08-05 | 增加 duration 到期统计摘要 | 可在停止发车时直接得到累计事务数、成功率、失败分类和字节数。 |
| 2026-08-05 | 发现 duration 后程序持续运行 | 原因是 main lcore 的 RX/TX 循环为无条件 `while (1)`；不影响已完成事务统计，但测试需手工终止。 |
| 2026-08-05 | 100 CPS 基线测试通过 | 120 秒内完成 11999 个 HTTP 短连接事务，成功率 100%。 |
| 2026-08-05 | 1000 CPS 测试出现 memzone 耗尽 | 每个短连接在 `nsock_alloc()` 中创建两个 DPDK ring，运行中出现 `Number of requested memzone segments exceeds maximum 2560`，导致大量 socket 创建失败。 |

## 4. 实测结果

### 4.1 基线：100 CPS，120 秒

场景参数：

```json
{
  "duration_sec": 120,
  "max_concurrency": 1000,
  "target_cps": 100
}
```

终端摘要：

```text
active=0 started=11999 done=11999 success=11999 fail=0
success_rate=100.00% fail_connect=0 fail_io=0 fail_proto=0
tx=443963 rx=1055912
```

| 指标 | 结果 |
|---|---:|
| 目标事务数 | 12000 |
| 实际 started / done / success | 11999 / 11999 / 11999 |
| 成功 RPS | 99.99 |
| 成功率 | 100.00% |
| 到期在途数 | 0 |
| 每事务应用层 TX / RX | 37 B / 88 B |
| 平均应用层 TX / RX | 3.7 KB/s / 8.8 KB/s |

结论：在 100 CPS 下，client、协议栈及 `192.168.21.106:8888` HTTP 服务可稳定完成短连接事务；该档位不能代表性能上限。

### 4.2 压力：1000 CPS，120 秒

场景参数：

```json
{
  "duration_sec": 120,
  "max_concurrency": 100,
  "target_cps": 1000
}
```

终端关键错误：

```text
EAL: memzone_reserve_aligned_thread_unsafe():
Number of requested memzone segments exceeds maximum 2560
RING: Cannot reserve memory
[CORE][ERROR] rte_ring_create(nsock) failed
```

终端摘要：

```text
active=1 started=119623 done=119622 success=75479 fail=44143
success_rate=63.09% fail_connect=44143 fail_io=0 fail_proto=0
tx=2792723 rx=6642152
```

| 指标 | 结果 |
|---|---:|
| 目标事务数 | 120000 |
| 实际尝试率 | 996.86 CPS |
| 成功 RPS | 628.99 |
| 成功率 | 63.09% |
| 失败率 | 36.91% |
| 到期在途数 | 1 |
| 每个成功事务应用层 TX / RX | 37 B / 88 B |

结论：scheduler 基本达到 1000 CPS 的尝试速率，但成功率被本地 DPDK ring/memzone 分配失败限制。该结果不能用于评价服务端或网络的 1000 CPS 能力。

`owner_io_socket_create()` 将 `nsock_alloc()` 的失败映射为 `EPROTONOSUPPORT`（errno 93），因此 `fail_connect` 在此测试中实际表示客户端本地 socket/ring 创建失败，而非远端拒绝连接。

## 5. 当前瓶颈与优化优先级

1. **P0：消除短连接热路径上的 per-socket DPDK ring 创建。** `nsock_alloc()` 当前为每个 socket 创建 `recv_buf` 和 `send_buf`；短连接反复创建/销毁会触发 memzone 段上限。owner-local traffic-gen flow 应改用预分配对象池，或采用可复用的 ring/buffer 池。
2. **P1：使测试自动收尾。** scheduler 停止且所有 flow 排空后，main lcore 应停止 RX/TX 循环、等待 worker、打印最终摘要并退出。
3. **P1：区分资源创建失败。** 为 socket/ring 分配失败使用独立错误码和统计项，不应计入远端 `fail_connect`。
4. **P2：增加延迟和峰值并发。** 每个 flow 记录起止 cycle，输出平均值与 P50/P95/P99；同步记录峰值 `active`。
5. **P2：增加窗口化统计。** 按报告周期输出 delta started、done、success、fail 和 RPS，便于定位瞬时退化。

## 6. 后续评测流程

每次优化后按以下阶梯复测，单档至少持续 120 秒：

1. 100 CPS：验证回归，要求成功率 100%、无资源错误。
2. 500 CPS：确认 memzone/ring 错误不再出现。
3. 1000 CPS：要求实际成功 RPS 接近目标、失败率为 0 或有可解释的服务端限制。
4. 继续逐档提高 CPS，直到出现持续排队、失败增长、CPU 饱和或网络达到瓶颈。

每次记录应包含：代码版本、完整命令、场景 JSON、CPU/NIC/hugepage 配置、服务端配置、duration 摘要和失败日志。
