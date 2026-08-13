# 2026-08-13：HTTP keep-alive 对照

本目录保存 2026-08-13 的复现实验入口和分析 Canvas 源码。实验固定
8 workers、目标 `100000 CPS`、`120 s`，只扫全局并发 `500/1000/5000`。
对端为 `192.168.21.106:8888`。

## 重跑测试

先在仓库根目录构建 `traffic-gen`，然后分别运行两种模式：

```bash
USE_SUDO=1 OVERWRITE=1 MODE=short \
  ./debug/2026-08-13/run_http_matrix.sh

USE_SUDO=1 OVERWRITE=1 MODE=keepalive \
  ./debug/2026-08-13/run_http_matrix.sh
```

默认输出到 `temp/`；脚本默认拒绝覆盖已有 CSV。没有 DPDK 权限需求时可用
`USE_SUDO=0`，更换二进制或输出目录可设置
`TRAFFIC_GEN_BIN=/path/to/traffic-gen`、`OUTPUT_DIR=/path/to/output`。

输出文件名为：

```text
8-13-8w-100000cps-500-con.csv
8-13-8w-100000cps-1000-con.csv
8-13-8w-100000cps-5000-con.csv
8-13-8w-100000cps-500-con-keepalive.csv
8-13-8w-100000cps-1000-con-keepalive.csv
8-13-8w-100000cps-5000-con-keepalive.csv
```

## 统计口径

- 500/1000 短连接档有 aggregate/final；5,000 短连接档和三份
  keep-alive CSV 使用最后一个完整 8-worker 快照，成功 RPS 按 120 秒计算。
- keep-alive 复用率按 `connections_reused / started` 计算，物理连接平均承载量
  按 `started / connections_created` 计算。
- 客户端 CSV 没有服务端 `accepted_queue` 深度或 overflow/drop 计数；该结论
  需要服务端队列计数和 SYN/RST 抓包进一步验证。

`traffic-gen-keepalive-comparison-2026-08-13.canvas.tsx` 是 Cursor Canvas
“Traffic Gen Keepalive Comparison”的仓库归档副本；可视化运行副本仍保留在
Cursor Canvas 管理目录。
