# 性能实验数据

新实验的日志、CSV、抓包、二进制、参数扫描结果及临时诊断脚本仅保存在本机，
不加入 Git。`.gitignore` 默认忽略本目录的新内容，仅保留本索引；历史已跟踪文件不受影响。
版本化结论见 [PERFORMANCE.md](../docs/PERFORMANCE.md)，架构记录见
[DEVLOG.md / ARC-009](../docs/DEVLOG.md#arc-009多-rxtx-队列与-worker-直接收发)。

## 本地数据位置

以下路径相对仓库根目录；新 clone 不包含这些本地文件。

| 目录 | 内容 |
| --- | --- |
| `debug/2026-09-16-main-lcore/` | 原路径基线、观测、旧对端诊断、通用采集脚本 |
| `debug/2026-09-16-relative/` | 时钟等局部开销对照 |
| `debug/2026-09-16-nuc/` | 新对端配置、18 轮收发模式 A/B、CSV/CPU/源码与二进制快照 |
| `debug/2026-09-16-tuning/` | 70 轮 workers/并发扫描、重复验证、逐 lcore 负载 |
| `debug/2026-09-16-rss-rootcause/` | Linux/独立 DPDK 强制 RETA→RXQ1、宿主机直连对照、抓包及恢复核对 |
| `debug/2026-09-17-nuc-rss/` | NUC I225-V 硬件 RSS 能力、2048 次 HTTP 的四队列分布及 DPDK 接管条件 |
| `debug/2026-09-17-nuc-deploy/` | 双机部署/回归、VFIO 接管及恢复、DPDK 四队列 UDP RSS、源码一致性校验 |
| `debug/2026-09-17-nuc-bench/` | NUC→HP HTTP/DNS、1/2/4 workers扫描、收发A/B、对端软件RPS、ARP丢首包修复前后、链路/CPU原始证据 |
| `debug/2026-09-20-short-bottleneck/` | 13轮NUC→HP短连接诊断、双端perf、RPS/对端睿频/conntrack/并发对照、超时扫描排除实验与恢复核对 |

每个实验目录内的 `README.md` 提供详细命令；`raw/` 保留未经聚合的记录。
汇总数值不得覆盖旧轮次，重测使用新目录。

## 复现口径

- **收发路径**：同一二进制比较 Main/Main、Main/worker、worker/worker；8 workers、
  并发 500，短连接及 keep-alive 各 3×30 秒，第二轮反转顺序。
- **参数扫描**：固定 Main RX + worker TX，扫描 workers 与全局并发；候选重复三轮。
  同时保留成功 RPS、失败类型、延迟、RX/TX 丢包与逐 lcore CPU，空轮询 CPU 不等于有效负载。
- **RSS 定位**：Linux `ethtool -l/-x/-S` 检查队列、分流表及计数；专用数据网卡执行
  `ethtool -X <接口> weight 0 1 0 0 0 0 0 0`，由不同 TCP 源端口发起流量，检查是否
  实际进入 RXQ1。DPDK 独立探针另核对 RSS 标记。配置读回匹配不能代替实际队列计数。
- **恢复与对比**：测试前保存驱动、地址、路由、RETA/offload，结束后恢复并核对；
  保存二进制 SHA256、源码版本、对端配置和统计窗口。跨日期提升不全部归因于代码。

本机现有离线核对入口为 `python3 debug/2026-09-16-rss-rootcause/summarize.py`；
收发 A/B 入口为 `debug/2026-09-16-nuc/run_ab.sh`，参数扫描计划见 tuning 目录。
这些临时工具随本地数据保留；测试时创建新的输出目录，避免覆盖本轮证据。
