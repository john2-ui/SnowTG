# owner_timer 后端验证（2026-09-28）

默认后端保持 `rte`。实现和使用说明见 [OWNER_TIMER.md](../../OWNER_TIMER.md)。

## 对照口径

- 原版：`9a01433` 的 `rte_timer`，worker 每 10 ms 轮询，flow 每 1 ms 全表扫描。
- rte：移除 flow 扫描、增加 flow timer、worker 每 1 ms 轮询。
- wheel：同一份候选源码，构建开关选择 1 ms 分层时间轮。

NUC I225-V → HP 既有 Nginx 8888 / DNS 1053，DPDK 26.07-rc3。
1 worker 使用 CPU 0、并发 512；4 workers 使用 CPU 0/2/4/6、并发 2048；
Main CPU 8，直接 RX/TX，EAL 1024 MiB。三个版本均使用 `-O3 -g -fno-omit-frame-pointer`。

先完成旧版 100k CPS 的六组预采样。该压力下有接纳资源不足、DNS 丢失和 keep-alive
EOF/RST，不能将其当成零错误容量结论。正式矩阵统一采用短连接 30k CPS、keep-alive
50k CPS、DNS 20k CPS，每轮预热 10 秒、稳态 30 秒，三次重复轮换后端顺序。
这些是固定供给速率的对照，不是最大吞吐搜索。

每轮用 `perf record -e cycles:u -F 199 -g` 采集稳态内部约 28 秒。
按 worker 线程的 sample period 汇总核心 cycles；一个样本只要调用栈包含
`owner_timer_*`、`rte_timer_*`、`__rte_timer_*`、`wheel_*`、`tg_flow_expire`
或 `tg_flow_timeout_*`，即计入 timer/flow 超时合计，重叠栈只计一次。
这涵盖扫描迁移到回调后的成本，不只比较某一个函数的 self 百分比。
每成功请求 cycles 用采样窗口与稳态成功速率估算，不是逐请求硬件计数。
DPDK busy-poll 的总 worker cycles 包括空轮询，不能直接解释为有效业务工作量。

切换门槛保持为：两种 worker 配置的 timer cycles 中位数至少降低 10%，且改善超过
重复波动；各正常场景成功吞吐退化不超过 3%、P99 恶化不超过 5%，无新增错误和资源残留。
证据不足或任一门槛未满足时保留 `rte` 默认。微基准不能单独触发默认切换。

## 双机正式结果与默认值

**最终默认保持 `rte`，时间轮实现和构建开关保留。** 54 轮全部完成，但 4 workers 的
三个场景均未达到 timer cycles 至少下降 10% 的门槛，微基准提升不能推翻此结果。
1 worker 的 DNS 重复范围也有重叠，尚不能认为其改善已超过测量波动。

下表为 timer/flow 到期合计 cycles / 成功请求，三轮中位数；方括号为最小、最大值。
差值比较 wheel 与迁移后的 rte。固定速率下，这与采样窗口合计 cycles 的相对变化近似一致。

| 场景 / worker 数 | 原版扫描 | rte [范围] | wheel [范围] | wheel 变化 |
| --- | ---: | ---: | ---: | ---: |
| HTTP 短连接 / 1 worker | 2661 | 2545 [1941, 2597] | 1574 [1259, 1731] | -38.2% |
| HTTP keep-alive / 1 worker | 1478 | 1369 [1169, 1511] | 1086 [859, 1102] | -20.7% |
| DNS / 1 worker | 2822 | 2047 [2008, 2086] | 1771 [1654, 2401] | -13.5% |
| HTTP 短连接 / 4 worker | 7293 | 4675 [4475, 5188] | 4698 [4572, 5223] | +0.5% |
| HTTP keep-alive / 4 worker | 4571 | 2856 [2744, 3106] | 2743 [2643, 2743] | -3.9% |
| DNS / 4 worker | 10832 | 6450 [5667, 6795] | 6326 [5918, 6920] | -1.9% |

总 worker cycles / 成功请求如下。总量包括 busy-poll，wheel 相对 rte 的中位数变化均小于
0.1%，不能将局部 timer 成本下降直接称为整体 CPU 或吞吐同比例提升。

| 场景 / worker 数 | 原版扫描 | rte | wheel |
| --- | ---: | ---: | ---: |
| HTTP 短连接 / 1 worker | 146182 | 146865 | 146747 |
| HTTP keep-alive / 1 worker | 87991 | 87719 | 87688 |
| DNS / 1 worker | 219867 | 219332 | 219372 |
| HTTP 短连接 / 4 worker | 466008 | 465945 | 466105 |
| HTTP keep-alive / 4 worker | 279522 | 279531 | 279689 |
| DNS / 4 worker | 699598 | 698297 | 698146 |

稳态成功速率三版均维持供给速率附近：短连接 30,000 RPS、DNS 20,000 RPS；
keep-alive 因 EOF/RST 略低于 50,000 RPS。wheel 相对 rte 的中位数退化均未超过 3%。
这只验证这些固定负载点，不构成最大可持续吞吐或饱和负载结论。

稳态成功请求 P99（ms）如下；中位数上的 wheel 最大退化为单 worker 短连接的 4.8%，
但重复范围和 keep-alive 的离群值说明不能忽略运行间波动。

| 场景 / worker 数 | 原版扫描 [范围] | rte [范围] | wheel [范围] |
| --- | ---: | ---: | ---: |
| HTTP 短连接 / 1 worker | 0.735 [0.703, 0.799] | 0.671 [0.671, 0.767] | 0.703 [0.671, 0.799] |
| HTTP keep-alive / 1 worker | 0.351 [0.351, 4.351] | 0.335 [0.335, 0.383] | 0.335 [0.319, 0.367] |
| DNS / 1 worker | 0.351 [0.351, 0.367] | 0.351 [0.351, 0.351] | 0.351 [0.351, 0.351] |
| HTTP 短连接 / 4 worker | 0.671 [0.639, 0.767] | 0.735 [0.639, 1.343] | 0.639 [0.607, 0.735] |
| HTTP keep-alive / 4 worker | 0.351 [0.335, 0.351] | 0.367 [0.335, 4.095] | 0.335 [0.319, 0.335] |
| DNS / 4 worker | 0.351 [0.351, 0.351] | 0.351 [0.351, 0.351] | 0.351 [0.351, 0.351] |

所有短连接/DNS 正式轮次无失败。keep-alive 的三轮总错误如下，仅出现 EOF/RST，不能把
launcher 的 `passed` 写成零错误通过。三版 54 轮均为零排空 socket 残留和零 TCP 强制清理。
HP 既有 Nginx 配置为 `keepalive_timeout 1s`、`keepalive_requests 1000`、
`reset_timedout_connection on`；此次未修改服务端配置，未声称已证明错误完全来自它。

| keep-alive 配置 | 原版 EOF / RST | rte EOF / RST | wheel EOF / RST |
| --- | ---: | ---: | ---: |
| 1 worker | 585 / 107 | 1298 / 140 | 858 / 83 |
| 4 worker | 896 / 130 | 585 / 63 | 572 / 44 |

单轮 timer 样本数为 42～366，采样百分比不是精确逐指令计数。
完整 54 轮数据见 [profile-summary.json](profile-summary.json)，三轮汇总与范围见
[profile-aggregate.json](profile-aggregate.json)。无新的容量上限声明，未进行小时级长测。

## 本地微基准

同一 VM、固定 CPU 2，轮换 rte/wheel 顺序三轮，每轮每项十次。
下表为 65,536 节点的 30 个样本中位数，单位为 timer cycles/操作；
`mixed` 包含 1 秒 / 1 小时期限混合的挂载、轮询与取消，总耗时除以节点数。

| 操作 | rte | wheel |
| --- | ---: | ---: |
| arm | 311.3 | 58.4 |
| rearm | 517.0 | 60.8 |
| cancel | 301.7 | 25.6 |
| 集中到期 | 127.2 | 42.9 |
| 有远期待办的 poll | 38.8 | 27.1 |
| 空 poll | 9.2 | 4.5 |
| mixed | 521.5 | 117.7 |

512、4096、65536 节点的完整汇总和范围见 [microbenchmark.json](microbenchmark.json)。
这是本机调度器开销，不是 NUC 的硬件核心 cycles，也不是网络吞吐提升倍数。

## 代码与本地验收

两种后端均通过现有完整回归与 ASan/UBSan 全套检查，包含 timer、flow、TCP、socket、workflow。
Sanitizer 设置为 `detect_leaks=0:abort_on_error=1` / `halt_on_error=1`；未声称通过 LeakSanitizer。

新增可控时钟检查覆盖毫秒边界、非整除时钟频率、不提前触发、全部八层迁移、长时间停顿、
`UINT64_MAX`、随机操作对照，以及回调释放自身/其他到期节点和立即重挂。
实际后端测试覆盖 owner 拒绝、容量耗尽与清理；flow 覆盖 UDP/TCP 超时、idle/FIN、重用、
多 map 隔离和 ready 完成撤销已排队超时。构建检查覆盖非法后端拒绝和同目录 rte→wheel→rte
重编译/链接；独立目录保留 A/B 二进制。

新增真实 `net_null` workflow CLI 回归：步骤 20 ms / 整体 1000 ms，以及步骤 1000 ms /
整体 20 ms，两者均应在 100 ms 内以响应超时完成并正常排空。修复前步骤用例实测拖到
约 1000.6 ms；修复后两个后端的 ASan/UBSan 二进制均通过。workflow 原先只写 flow 的
`deadline_cycles`，迁移后必须同步 `owner_timer_arm_at()`；挂载失败沿既有关闭路径退出。

初次构建 flags 错误、并行测试争用同一个 DPDK file-prefix 的失败日志，以及修复前的
workflow 回归失败均保留；修正后顺序复测通过，不从原始材料中删除失败轮次。
NUC 首次验收还暴露 scenario/connection-pool 测试缺少 DPDK include flags，已补齐并复测；
第二次构建被共享工作区正在修改的资源计数源码打断，第三次从冻结快照验收。
perf 文件转交所有权后的读取检查及后台服务 shell 占用 SSH 输出也已修复；均保留日志。

## 双机功能验收与恢复

最终冻结源码的 rte / wheel 两种后端均通过 NUC 完整回归、11 个真实网卡 workflow 用例、
无网卡 workflow CLI 的步骤/整体超时检查，以及 HTTP/DNS 故障注入与恢复。
22 个 workflow 运行检查成功/预期失败、分支、提取、步骤期限、整体期限和跨阶段完成计数；
4 个故障恢复运行在正常和恢复阶段成功率均为 100%，仅产生预期的响应超时。
全部 26 个运行均为零 socket 排空残留、零 TCP 强制清理。

| 后端 | HTTP 响应超时数 | DNS 响应超时数 | 正常 / 恢复阶段 |
| --- | ---: | ---: | --- |
| rte | 60 | 56 | 100% / 100% |
| wheel | 56 | 56 | 100% / 100% |

故障注入窗口由控制端墙钟控制，以上次数差异不作为后端性能比较。
完整结果摘要见 [live-summary.json](live-summary.json)。

额外使用 GDB 仅在退出函数设置断点，直接读取 timer 和 flow pool 计数，不改变产品接口。
两个后端各运行 2 workers、40 次混合请求：20 成功、10 预期连接 RST、10 UDP 响应超时，
started=done=40。每个 worker 的 timer active=0，flow pool free=capacity=64；
最终 active、live_sockets、tcp_drain_residual、tcp_forced_cleanup、tcp_pool_objects_in_use
全部为 0，程序正常退出。见 [直接计数记录](drain-counters-summary.json)。
这些是短期终态验收，不是小时级泄漏或资源峰值/趋势验证。

实验结束已核对 NUC 的 `igc` 驱动、`192.168.10.86`、大页数 0，临时恢复 timer 和
发流进程均已退出；HP 独立测试服务已停止，fault 配置已清空。恢复后从 NUC 访问
HP 8888 返回 HTTP 200，1053 DNS 返回 rcode 0 和一条答案，既有服务未修改。

## 源码对应关系

正式 profile 候选源码指纹为
`a142e80d0b4a0e760306514885fdc44a59f57d490092a63b561b848f72432611`；
最终验收源码指纹为
`9d17fb5c494517161582320723aaece2cea7e234dce1dd881c8f48cd54f57307`。
原生 C/H 差异只有上述 `workflow.c` 步骤期限修复。profile 使用的 legacy HTTP/DNS 场景
不进入 workflow 路径；仍分别保留 profile 和最终验收二进制、元数据，不把两种哈希混写。
后者未重新进行完整 54 轮性能采样，这也是不切换默认后端的保守边界。

验收期间共享工作区的另一项资源计数任务继续修改了 timer/workflow 等文件；这些改动
全部保留。本报告使用 `acceptance-source/` 中逐文件校验过的冻结源码，结论只覆盖上述
哈希，不能用来声明后续并行改动也已通过相同验收。既有未跟踪建议文档未编辑，未提交 Git commit。

## 复现与原始材料

- `debug/2026-09-28-owner-timer/` 保存本机源码快照、构建和测试日志、微基准原始 CSV。
- NUC `/home/lca/work/snowtg-owner-timer-20260928/` 的 `baseline/`、`rte/`、`wheel/`
  是独立源码与构建目录；`runs-pre/`、`runs-full/` 分开保存压力预采样和正式矩阵。
- 三版 profile 二进制和构建参数见 [builds.json](builds.json)，原始文件另有 SHA256 清单。
- 每轮保留 `perf.data`、展开栈、原生 CSV、完整 `result.json`、日志及构建元数据。
  准备新目录重测，不覆盖已有输出；哈希不能替代原始数据和被测二进制。
- [run-profile.py](run-profile.py) 在 NUC 以 root 运行，参数为 `pre` 或 `full`。
  它为实验网卡/巨页设置恢复 timer，并在结束时恢复。此脚本固定上述实验机路径及网卡，
  复用到其他机器前必须修改配置；它不会修改 HP 的既有服务。
- [summarize-profile.py](summarize-profile.py) 在 NUC 接受 `runs-full` 路径，解析 perf 栈
  并生成 `profile-summary.json`；[aggregate-profile.py](aggregate-profile.py) 接受该 JSON，
  汇总三轮中位数及范围。原生场景结果中的 `passed` 不能代替逐项错误检查。
- [run-live.py](run-live.py) 在冻结验收源码根目录运行，参数为新的本地输出目录，交互读取 NUC sudo
  密码，不落盘。需要已建立 `/tmp/snowtg-acceptance-wifi` 和 `/tmp/snowtg-acceptance-hp`
  SSH control socket，以及上述源码镜像和 HP 独立测试服务脚本。
- [check-drain.sh](check-drain.sh) 和 [drain-counters.gdb](drain-counters.gdb) 在固定实验机上
  检查退出时的 flow pool / timer 计数，使用 [混合场景](drain-counters.json)。
  只在前一轮恢复完网卡后运行；它复用 `live-r3/restore.sh`，不能脱离该实验目录直接使用。
