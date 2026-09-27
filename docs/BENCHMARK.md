# SnowTG / dperf / wrk：同环境对照与容量边界

本页保留 2026-09-26 原有 **60 个有效样本**，并新增同日短连接六轮正式对照及升压诊断。图示每个条件三轮；新增与原有条件分开说明。完整原始日志保留在实验工作区，仓库收录可核验摘要、逐轮数据和图表。

## 结论

- 普通 KA 双 worker：SnowTG **393,900 RPS**、dperf **391,490 RPS**、wrk **400,314 RPS**。三轮波动范围重叠，且按工具分批运行，无法据均值微小差异判断胜负。
- CPU 时间受限：20% / 40% 预算下，dperf 的请求吞吐约为 SnowTG 的 **6.37 / 7.49 倍**；同时存在 RST/重传。普通组接近不代表 CPU 效率相同。
- 新增 120k CPS / 16 端口短连接：SnowTG **117,916 RPS**，dperf **119,997 RPS**，约差 1.7%；存在资源延期与 socket 异常，不代表无损极限。
- 固定 16k 短连接：SnowTG 三轮各 480,000 次成功，失败、skipped、资源延期均为 0。它是固定到达率回归，不是最大 CPS。
- 常规与连续限频的 SnowTG 默认配置 18 轮累计成功 **100,769,044**、失败 **0**，TX / 软件 RX 丢弃、统计丢失及退出残留均为 0。强配额过载组不包含在此结论中。

![常规饱和吞吐](assets/benchmark-capacity-zh.svg)

![CPU 时间预算对照](assets/benchmark-cpu-budget-zh.svg)

## 原有 60 个样本：环境和负载

| 项目 | 本次设置 |
| --- | --- |
| 发流端 | Intel NUC12WSKi5，i5-1240P，12 核 / 16 线程，15.6 GiB；I225-V，DPDK / VFIO |
| 服务端 | HP 15-fc0xxx，Ryzen 7 7730U，8 核 / 16 线程，约 15 GiB；RTL8153 USB 3 千兆网卡，1 RX / 1 TX |
| 网络 | 192.168.10.86 → 192.168.10.234；实测协商 1 Gbps，全程监测链路；不能按 I225-V 标称 2.5 Gbps 解读 |
| nginx | 16 workers，软件 RPS 掩码 5155；keepalive_timeout 1 s，keepalive_requests 1,000,000,000；响应体 `ok\n`，3 B |
| HTTP | HTTP/1.1 GET `/`；KA 请求 93 B，短连接请求 88 B；相同 Host、请求内容和服务端 |
| CPU 布局 | 单 worker CPU 0，双 worker CPU 0、2；DPDK main CPU 8；普通组约 4.4 GHz |
| 容量 | 普通组并发 512；DPDK 各 2 GiB；单源 IP、源端口 49152～65535，共 16,384 个 |
| 网卡描述符 | 普通组 SnowTG 默认 RX/TX 各 1024，dperf 各 4096；CPU 配额组统一 4096 |
| 发流策略 | SnowTG 饱和组 target_cps=1,000,000、无限复用；dperf KA cps=8000、cc=512、keepalive=0us；固定短连接双方 cps=16000 |

相同链路、请求、服务端、并发与 worker 数构成共同条件；普通组描述符默认值、应用调度方式及内核网络成本并非完全相同。wrk 的线程固定在对应 CPU，但 IRQ/软中断可使用额外 CPU，所以不加入 DPDK worker 的严格配额比较。dperf 使用 HTTP_PARSE 构建、`protocol http`，不是绕过 HTTP 解析的 TCP 负载。

### 时间窗口与错误口径

普通组配置时长 30 s，共同稳态窗口为发流起点后 **12～27 s**。dperf 另有 10 s slow-start，实际进程运行时间包含初始化和收尾，不等于配置时长。每条件三轮，报告均值及 min～max，不把范围当作置信区间。最新 SnowTG 与未变化的 dperf 对照按工具批次运行，并非完全随机交错。

**RPS 是 nginx 收到的请求速率**，扣除监控请求；不是客户端成功完成速率。PPS 为 HP 网卡硬件计数，RX 对应客户端→服务端，TX 对应服务端→客户端，包含少量背景流量与重传。正常 KA 每请求约一个客户端包，RPS 与 PPS 接近是结果，不是用 RPS 换算 PPS。

错误是各工具的**全程原生计数**，包括热身和收尾，运行时长/定义不同，不能直接比较失败率。SnowTG 使用 fail；wrk 使用 status/connect/read/write/timeout；dperf 同时查看 httpErr、skErr、rstRx、pushRt、tcpDrop、imissed。其 RST 关闭不增加 HTTP/socket error，httpGet 也可能包括 PUSH 重传；不能把 RST 与 error 简单相加当成精确失败事务数。

SnowTG 原生 success / 配置 duration、dperf 原生日志 seconds 12～26 的 http2XX 均值、wrk 全程 Requests/sec 的窗口不同，仅保留在数据中供复核，不混入图表。饱和发流的 skipped 表示超过容量的计划到达被跳过，不能当成网络失败；固定 16k 组则要求 skipped 为零。

### 如何限制客户端 CPU

先测单 worker 连续 400 / 800 MHz，再测 **400 MHz + CFS v1 时间配额**。配额组在连接热身后使用 2048 并发，并将两工具 RX/TX 描述符统一为 4096；SnowTG 是临时 `aligned3` 构建，仓库默认 1024 不变。该构建另在退出时打印 NIC 计数，不在热路径添加采样逻辑。

配置 duration=45 s，dperf 另有 slow-start。就绪后等待 10 s，将 CPU 0 上的 worker TID 加入单独 cgroup，main 不限；取配额生效后 **5～20 s** 为共同窗口。10% / 20% / 40% 的 quota/period 分别为 1000/10000、1000/5000、1000/2500 μs。三轮中第二轮反转工具顺序。

20% 组实际 worker CPU 时间为 SnowTG / dperf **0.198 / 0.201 核**，40% 为 **0.388 / 0.389 核**，以 cpuacct.usage 为准。预算翻倍 RPS 分别增长 1.92 / 2.26 倍；40% 组服务端最忙逻辑 CPU 的窗口平均占用约 9.1% / 48.7%，带宽低于 256 Mbps。这支持客户端 CPU 时间预算成为主要限制，但调度暂停、队列溢出和连接恢复仍参与结果，不能解释成纯指令成本或独占物理核的无损极限。

## 全部正式组结果

下表错误均为三轮全程合计，RPS 和 PPS 为三轮共同窗口均值；PPS 单位为千包/秒。逐轮值见 [JSON](benchmarks/2026-09-26.json)。

| 条件 | 工具 | RPS 均值（min～max） | 客户端→服务端 kPPS | 服务端→客户端 kPPS | 原生错误 / RST |
| --- | --- | ---: | ---: | ---: | --- |
| 固定 16k 短连接 / 2w | snowtg | 15,999（15,999～15,999） | 74.3 | 80.0 | 0 |
| 固定 16k 短连接 / 2w | dperf | 16,000（16,000～16,001） | 64.0 | 80.0 | 0 / RST 0 |
| KA / 2w | snowtg | 393,900（316,295～437,762） | 393.9 | 393.9 | 0 |
| KA / 2w | dperf | 391,490（343,626～444,217） | 391.5 | 391.5 | 0 / RST 0 |
| KA / 2w | wrk | 400,314（351,630～434,569） | 400.3 | 400.3 | 0 |
| KA / 1w | snowtg | 395,202（304,370～443,113） | 395.2 | 395.2 | 0 |
| KA / 1w | dperf | 386,352（314,360～435,593） | 386.3 | 386.3 | 0 / RST 0 |
| KA / 1w | wrk | 289,737（277,734～295,912） | 289.7 | 289.7 | 0 |
| KA / 400 MHz | snowtg | 77,902（77,738～78,017） | 77.9 | 77.9 | 0 |
| KA / 400 MHz | dperf | 450,187（444,824～453,044） | 450.2 | 450.2 | 0 / RST 0 |
| KA / 800 MHz | snowtg | 139,163（137,513～142,457） | 139.2 | 139.2 | 0 |
| KA / 800 MHz | dperf | 395,768（339,837～438,789） | 395.7 | 395.7 | 0 / RST 0 |
| KA / 10% 配额 | snowtg | 5,142（5,122～5,177） | 5.3 | 5.4 | 878 |
| KA / 10% 配额 | dperf | 35,567（33,397～37,262） | 36.5 | 38.2 | 93 / RST 35,062 |
| KA / 20% 配额 | snowtg | 11,166（11,153～11,189） | 11.2 | 11.2 | 0 |
| KA / 20% 配额 | dperf | 71,131（70,643～71,587） | 72.5 | 74.2 | 47 / RST 59,432 |
| KA / 40% 配额 | snowtg | 21,464（20,864～22,074） | 21.5 | 21.5 | 0 |
| KA / 40% 配额 | dperf | 160,670（156,941～164,083） | 161.9 | 163.4 | 0 / RST 53,210 |
| 饱和短连接 / 2w | snowtg | 117,546（116,177～120,094） | 505.7 | 587.9 | 0 |
| 饱和短连接 / 2w | wrk | 16,350（15,667～16,840） | 81.8 | 81.7 | 0 |

### 过载与未测得的极限

| 条件 | SnowTG | dperf | 解释 |
| --- | --- | --- | --- |
| 10% 配额，2048 并发，三轮 | fail 878；NIC missed 14,338 | HTTP/socket error 93；RST 35,062；PUSH 重传 40,521 | 两者均出现稳定性问题；保留全部样本 |
| 20% 配额，2048 并发，三轮 | fail / NIC missed 均为 0 | HTTP/socket error 47；RST 59,432；PUSH 重传 61,503 | dperf 吞吐更高，但不是零异常 |
| 40% 配额，2048 并发，三轮 | fail / NIC missed 均为 0 | HTTP/socket error 0；RST 53,210；PUSH 重传 53,863 | 原生 error=0 不能代替重传/重置检查 |
| 20% 配额、2048 并发，SnowTG 默认 1024 描述符，单轮诊断 | 9,477 RPS；fail 2,645；NIC missed 151,764 | — | 与 4096 描述符对照说明接收队列容量影响过载恢复 |
| 20% 配额、4096 并发、4096 描述符，单轮诊断 | 6,209 RPS；fail 8,636 | 79,574 RPS；socket error 443；RST 60,714 | 增加并发会放大排队与恢复压力；不纳入正式三轮均值 |

正式配额组 dperf 的 RST/PUSH 重传在稳态窗口内仍持续出现，不能全部归因于热身或退出。NIC missed 只计 rx_missed_errors（dperf 为 imissed），不重复累加同义或重叠计数。

400 MHz 连续限频时 dperf 仍达约 450k RPS，服务端最忙 CPU 约 90.4%，因此仅降频不足以同时隔离两者的发流 CPU 瓶颈。普通组的最大单轮值只是**测得值**，不是协议栈能力上限；没有将单次峰值取代三轮均值。

短连接饱和组 SnowTG 均值 117,546 RPS、客户端约 4.30 包/请求；固定 16k 时约 4.64，对照 dperf 约 4。原有 dperf 受单 IP、小端口池的启动容量校验限制，没有同模型的无速率上限短连接有效样本；新增 16 端口、固定 120k CPS 对照见下节，仍不等同于无速率上限测试。wrk 不提供原生固定到达率控制，因此没有固定 16k 组。wrk 饱和短连接共同后段窗口为 16,350 RPS，但原生全程均值为 44,709 RPS，存在早快后慢；本次未确定唯一根因，不能用后段窗口宣称 SnowTG 普遍快七倍。


## 新增短连接测试：120k CPS 对照与升压诊断

![短连接分组对照](assets/benchmark-short-zh.svg)

2026-09-26 补测，SnowTG 使用仓库提交 `0f081eb` 重新构建，包含提交前的部分发送失败保护；二进制 SHA-256 为 `65197da9544c218730a67c28cae6347bee2feb7eedd82d40a775e645738efb25`。dperf 沿用上文 `69998e5` 的 HTTP_PARSE 构建。机器、1 Gbps 链路、CPU 0/2 双 worker、main CPU 8、2 GiB 内存、88 B GET 与 3 B 响应保持一致，未限制 CPU 时间或降低频率。

为满足 dperf 短连接启动时的 socket 容量校验，双方均使用 **16 个目标端口**，每轮换一组端口，源 IP 与 49152～65535 源端口范围相同。双方目标 120,000 CPS；SnowTG 每连接只发一个请求、16 个等权 HTTP class、并发上限 512、socket-id-max 16,384；dperf 短连接模式拒绝 `cc` 配置，只配置 `cps`，不限制为相同并发。目的端口扩展增加了四元组空间，**不能称为完全相同的连接容量配置**。RX/TX 描述符仍为 SnowTG 1024、dperf 4096。

服务端最初出现 `nf_conntrack: table full, dropping packet`，上限 262,144。确认后仅对两台测试 IP 间、目标端口 20000～21999 的 TCP 流量临时设置双向 NOTRACK；先前各轮列为排除的诊断数据，不用于正式均值。新图的 16 端口组使用该设置，原有单端口 SnowTG/wrk 组未采用此设置，**不可跨组比较**。NOTRACK 后的探索轮仍有波动，不能把此前所有异常都归因于连接跟踪。

配置时长 30 s，dperf 另有 10 s 慢启动；共同窗口依旧为发流起点后 12～27 s，RPS 是服务端收到请求速率，PPS 为网卡计数。正式样本按时间顺序为 dperf、SnowTG、dperf、SnowTG、SnowTG、dperf；各取三轮，不用单次最佳值替代均值。

| 工具 | RPS 均值（最小～最大） | 客户端→服务端 kPPS | 服务端→客户端 kPPS | 三轮全程原生计数 |
| --- | ---: | ---: | ---: | --- |
| SnowTG | 117,916（117,336～118,227） | 529.6 | 589.6 | success 10,585,539；fail / skipped 均 0；resource deferred 214,461 |
| dperf | 119,997（119,993～120,000） | 480.0 | 600.0 | httpErr 0；skErr 16；RST 接收 0；NIC missed 0 |

SnowTG 平均吞吐比 dperf 低约 **1.73%**，每请求客户端包数约 **4.49 / 4.00**。三轮共计划 10,800,000 次到达，SnowTG 资源延期部分未在配置运行时间内完成；**零 fail 不代表所有计划负载达标**。dperf 另记录 RST 发送 6,089、SYN/FIN/PUSH 重传合计 101/104/30、tcpDrop 409；这些全程计数定义不同，不相加作为失败事务数，也不与 SnowTG 直接比较失败率。

升压探索也保留在[逐轮数据](benchmarks/2026-09-26-short.json)中：

| 目标 CPS | SnowTG（探索并发上限 8192） | dperf | 解释 |
| --- | --- | --- | --- |
| 96k，各一轮 | 21,797 RPS；fail 10,071 | 68,004 RPS；skErr 11,937；RST 接收 1,214 | NOTRACK 后仍有异常，根因未隔离；不能据这轮确定容量 |
| 120k，探索一轮 | 120,012 RPS；fail / skipped / deferred 均 0 | 正式三轮如上 | 增大并发可在单轮达到目标，尚无三轮无异常确认 |
| 160k，各一轮 | 112,476 RPS；fail 34；skipped 1；deferred 1,413,224 | 121,680 RPS；skErr 2,827；RST 接收 372,193 | 双方异常，不能把目标速率视为稳定处理能力 |

结论限于 **120k CPS 固定负载下吞吐接近**。目前尚未确定双方绝对极限，也未隔离发流端 CPU、服务端及连接回收各自的限制；96k 探索异常说明还有复现与稳定性问题待查。简历可以分别描述长连接吞吐相近、短连接此负载点接近 dperf，以及此前单端口对照中高于 wrk，不能写成“短连接极限与 dperf 相同”。

原始逐秒日志、配置、运行命令、服务器诊断与分析脚本位于 `/home/snow/snowtg-short-20260926/`；归档 JSON 包含正式、探索及排除样本、配置和原始文件哈希。测试后已恢复 NUC 的 igc 驱动、有线地址、巨页和路由，恢复 HP 的 nginx/RPS 并删除临时 NOTRACK 规则，8888 服务返回 HTTP 200。

## 2026-09-26 修复与验证记录

| 问题 / 设计点 | 实现方式 | 结果或边界 |
| --- | --- | --- |
| 多核状态归属 | 每个连接由一个 owner 驱动，per-core reactor、owner-local 队列；按需内存与 Dirty TX 减少空扫描 | 热路径避免共享锁；支持 worker RX/TX |
| 固定 16k 端口耗尽 | 分离 HTTP 完成与 TCP 关闭；短连接收到完整响应后，在原截止时间内等待对端 FIN | 避免不必要的主动关闭与 TIME_WAIT 堆积；三轮各 480,000 次成功，skipped / resource deferred 均为 0 |
| KA 复用失效连接 | 完整响应伴随 EOF/HUP 时禁止复用；复用前检查连接状态，仅在未发送新请求字节前替换失效连接 | 避免误用关闭连接，不自动重放已经发出的请求 |
| 每连接最多 100 次 | 新增 `--max-requests-per-connection N`；默认 `0` 不限，`1` 不复用，`100` 保留旧策略 | 可按目标服务的连接策略配置，计数包含首个请求 |
| 重复 ACK | 合并同一 owner 轮次的 ACK / 接收窗口更新，优先随数据、重传或 FIN 发出；无数据时同轮补纯 ACK | KA 客户端包数约 3 → 1；保留乱序、重复包、FIN 的及时反馈 |
| NIC 暂时发不出去 | TX 队列只提交网卡已接受的包，未发送部分保留 FIFO，下轮重试 | 避免部分发送时直接释放并丢包 |
| 丢包恢复迟缓 | 有有效 RTT 样本后 RTO 下限降至 200 ms，保留初始 1 s、Karn 与退避 | 属于低延迟策略取舍；可用编译宏恢复 1 s 下限 |

本轮代码已通过协议栈、发生器和示例构建、完整测试，以及针对 flow / TCP ACK 的 ASan/UBSan 检查。测试覆盖连接复用、EOF、HTTP 截断、ACK 捎带与回退、TX 部分接受及 FIFO/内存回收。

**后续重点**：CPU 热路径剖析、极端暂停下的 RX 队列与连接恢复、短连接额外 ACK。此次未宣称所有过载失败已消除，也尚未在更强服务端/链路上确定物理单核的无损极限。TLS、更复杂响应与长期丢包/乱序负载仍需要独立验证。

## 版本、证据与复现

| 工具 / 构建 | 版本与说明 | 二进制 SHA-256 |
| --- | --- | --- |
| SnowTG `ack3` | 基于 `c2b9586618f600987ca6d915af676502ed6b35ac` 的本轮未提交修复快照；源码哈希清单见下方 | `d7e80d68a2cb04b234f006718b1fbc6f796872ac6410848407ed2078aaeaedbe` |
| SnowTG `aligned3` | 同一修复版，仅临时调整描述符为 4096 及退出 NIC 统计 | `b49f356827cf4da1361d2b8b481b78f3d3424579f1201164f2d8b16a94b7e0a8` |
| dperf | `69998e55f0d9e968e3d53dcb7e1b32d8094144e8`；仅去除错误的重复 payload 路径启动检查，不修改 TCP / 统计热路径 | `57e21e3b1638ab683d096b9e204206977d72d344fb4230afc7d9f1d09b3a1bad` |
| wrk | `a211dd5a7050b1f9e8a9870b95513060e72ac4a0` | `560fea75fc1662fb5a591a3cefe3ba205b1d6d683cc6cbb8bd61c71cdd9911d8` |

提交前 review 另补充了普通 HTTP 调度路径的保护：只有发送前检测到 `ESTALE` 才替换连接，其他复用发送失败直接报告，避免部分发送后自动重放。本页硬件结果对应此前归档快照，不包含这项边界修复的重新测量。

两个 DPDK 工具均使用私有安装的 DPDK 26.07-rc3。wrk Lua 只配置线程亲和性、静态请求头和结束统计，没有逐请求 Lua 回调。

[逐轮数据](benchmarks/2026-09-26.json)保留全部 60 个正式样本的指标、工具原生计数、实验 ID 与原始文件哈希；[源码清单](benchmarks/2026-09-26-source-sha256.json)标识被测 SnowTG 快照。仓库未打包大体积抓包、逐秒日志和二进制，哈希用于后续核对，不等同于随仓库提供完整原始证据。完整日志位于原实验机 `/home/snow/snowtg-fix-20260926/compare/`，先前临时报告位于 `/home/snow/snowtg-compare-20260926/SnowTG-wrk-dperf-临时对比.md`。

重绘中英文图表使用随仓库提供的数据及 matplotlib，无需实验机器。中文绘图还需要 Noto Sans CJK SC 字体；可安装该字体，或通过 `--cjk-font /path/to/NotoSansCJKsc-Regular.otf` 指定已有字体文件。SVG 内嵌字形，阅读图片无需安装字体：

```bash
python3 docs/benchmarks/plot.py
```

硬件复测按前述环境设置服务端、绑定 CPU / NIC、核对 1 Gbps 链路后再运行。代表性普通 KA 配置如下，路径和端口须按实际机器替换；双方必须指向同样的响应端点。启动前以实际 HTTP 请求验证端点就绪。

```text
# dperf：HTTP_PARSE 构建；keepalive.http 为与 SnowTG 完全一致的 93 B 请求
mode client
cpu 0
protocol http
duration 30s
socket_mem 2048
port 0000:64:00.0 192.168.10.86 192.168.10.234
client 192.168.10.86 1
server 192.168.10.234 1
listen 19511 1
cps 8000
cc 512
keepalive 0us
client_port_range 49152 65535
payload_file /path/to/keepalive.http
rss
slow_start 10
```

SnowTG 剧本：

```json
{
  "name": "compare-keepalive",
  "duration_sec": 30,
  "max_concurrency": 512,
  "target_cps": 1000000,
  "report_interval_sec": 1,
  "classes": [{
    "name": "http_get", "weight": 1, "transport": "tcp",
    "peer": {"ip": "192.168.10.234", "port": 19511},
    "http": {"method": "GET", "path": "/", "host": "snowtg-wrk-dperf-benchmark.internal.example", "keepalive": true}
  }]
}
```

```bash
# 普通单 worker；需要已准备好巨页和 VFIO 的权限环境
traffic-gen/build/traffic-gen -l 8,0 --main-lcore 8 -a 0000:64:00.0 \
  --file-prefix snowtg-benchmark -m 2048 -- --workers 1 \
  --max-requests-per-connection 0 --socket-id-max 16384 \
  --rx-mode worker --tx-mode worker --local-ip 192.168.10.86 scenario.json
```

HTTP 原始请求为以下内容，**每行使用 CRLF，最后再加一个空行**；KA 总计 93 B：

```http
GET / HTTP/1.1
Host: snowtg-wrk-dperf-benchmark.internal.example
Connection: keep-alive

```

复测时同步采集 nginx 请求计数、双向 NIC 包/字节计数、worker CPU 时间/频率、链路速度及客户端全程错误；以共同稳态窗口计算速率，至少重复三轮。端点未就绪或链路异常的预检不得计入正式样本。压测已在原任务结束时停止，两台机器的网卡、频率、nginx、巨页及临时 cgroup 均已恢复。
