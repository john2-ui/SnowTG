# 2026-08-12：并发扫参 + SYN 抓包

## 问题

抬高 `max_concurrency` 后，HTTP 短连接 **RPS 不升反降、complete 变慢**——是本机瓶颈还是对端/路径？

## 过程

1. **日志扫参**：固定 8 workers、`target_cps=8000`，扫 250/500/750 并发（剧本见本目录 `http-sweep-*.json`；原始日志 `result`）。
2. **抓包对照**：稳态短抓 SYN/FIN/RST → `syn_rtt.sh` 比 SYN→SYN-ACK。
3. **判定**：本机无 ENFILE/端口耗尽；750 握手长尾+重传恶化 → 过载在对端/路径。

```bash
# 扫参（从仓库根）
cd traffic-gen
sudo ./build/traffic-gen -- --workers 8 ../debug/2026-08-12/http-sweep-8kcps-250con.json
# …500 / 750 同理

# 抓包后统计
../debug/2026-08-12/syn_rtt.sh /tmp/sweep-250.pcap 192.168.21.106 8888
../debug/2026-08-12/syn_rtt.sh /tmp/sweep-750.pcap 192.168.21.106 8888
```

## 扫参结果（~120s）

| 并发 | started CPS | 成功 RPS | fail | complete p50 | 备注 |
| ---: | ---: | ---: | ---: | ---: | --- |
| 250 | ~1205 | ~1205 | 2 | ~42 ms | 相对最好 |
| 500 | ~1151 | ~1151 | 7 | ~106 ms | |
| 750 | ~1138 | ~1138 | 40 | ~158 ms | rto-give-up↑ |

未打到 8k CPS；跑在并发上限，吞吐 ≈ 并发/时延。

## 抓包 SYN→SYN-ACK

| 档 | unanswered | SYN retrans flows | p50 / p99 |
| --- | ---: | ---: | --- |
| 250 | 0.25% | 0 | 19 / 35 ms |
| 750 | 0.57% | 2454 | 29 / **1057** ms |

## 结论

客户端槽位/发包不是主因；**对端/路径在高并发下握手变慢（长尾+重传）**，占满并发槽拖垮 RPS。下一步查对端 accept/CPU
