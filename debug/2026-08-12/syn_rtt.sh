#!/usr/bin/env bash
# syn_rtt.sh — 从 pcap 统计 TCP SYN→SYN-ACK RTT 与无响应 SYN 比例
#
# 用法:
#   ./debug/2026-08-12/syn_rtt.sh <pcap> [host] [port]
#
# 示例:
#   sudo tcpdump -i eth0 -n -s 128 -w /var/tmp/sweep-250.pcap \
#     'host 192.168.21.106 and tcp port 8888 and (tcp[tcpflags] & (tcp-syn|tcp-fin|tcp-rst) != 0)'
#   sudo chown "$USER" /var/tmp/sweep-250.pcap   # AppArmor 下 tshark 常读不了 tcpdump 属主的文件
#   ./debug/2026-08-12/syn_rtt.sh /var/tmp/sweep-250.pcap 192.168.21.106 8888
#
# 依赖: tshark, python3
#
# 说明: Ubuntu AppArmor 限制 tshark 可读路径（多为 /tmp、/var/tmp），且对非属主
# pcap 可能 EACCES。本脚本在探测失败时会复制到 /var/tmp 再分析。

set -euo pipefail

usage() {
  echo "usage: $0 <pcap> [host] [port]" >&2
  exit 2
}

[[ $# -ge 1 && $# -le 3 ]] || usage

PCAP=$1
HOST=${2:-}
PORT=${3:-}
CLEANUP_PCAP=

cleanup() {
  if [[ -n "${CLEANUP_PCAP}" && -f "${CLEANUP_PCAP}" ]]; then
    rm -f "${CLEANUP_PCAP}"
  fi
}
trap cleanup EXIT

if [[ ! -f "$PCAP" ]]; then
  echo "error: pcap not found: $PCAP" >&2
  exit 1
fi

if ! command -v tshark >/dev/null 2>&1; then
  echo "error: tshark not found (install wireshark-cli / tshark)" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "error: python3 not found" >&2
  exit 1
fi

# AppArmor: tshark may refuse paths outside tmp, or files owned by tcpdump.
if ! tshark -r "$PCAP" -c 1 >/dev/null 2>&1; then
  if [[ ! -r "$PCAP" ]]; then
    echo "error: cannot read $PCAP (permission denied at filesystem)" >&2
    exit 1
  fi
  COPY="/var/tmp/syn_rtt.$$.pcap"
  if ! cp -f "$PCAP" "$COPY" 2>/dev/null; then
    echo "error: tshark cannot open $PCAP (AppArmor?), and copy to $COPY failed" >&2
    echo "hint: sudo chown \"\$USER\" \"$PCAP\"  or  cp to /var/tmp then re-run" >&2
    exit 1
  fi
  if ! tshark -r "$COPY" -c 1 >/dev/null 2>&1; then
    echo "error: tshark still cannot open pcap after copy to $COPY" >&2
    echo "hint: check AppArmor profile /etc/apparmor.d/tshark" >&2
    rm -f "$COPY"
    exit 1
  fi
  echo "note: tshark could not read $PCAP; analyzing copy $COPY" >&2
  PCAP=$COPY
  CLEANUP_PCAP=$COPY
fi

filter='tcp.flags.syn==1'
if [[ -n "$HOST" ]]; then
  filter+=" && ip.addr==${HOST}"
fi
if [[ -n "$PORT" ]]; then
  filter+=" && tcp.port==${PORT}"
fi

# fields: stream  time  syn  ack  src  sport  dst  dport
tshark -r "$PCAP" -Y "$filter" -T fields \
  -e tcp.stream \
  -e frame.time_epoch \
  -e tcp.flags.syn \
  -e tcp.flags.ack \
  -e ip.src \
  -e tcp.srcport \
  -e ip.dst \
  -e tcp.dstport \
| SYN_RTT_PCAP="$PCAP" SYN_RTT_HOST="$HOST" SYN_RTT_PORT="$PORT" python3 -c '
import os
import sys

pcap = os.environ.get("SYN_RTT_PCAP", "")
host = os.environ.get("SYN_RTT_HOST", "")
port = os.environ.get("SYN_RTT_PORT", "")

streams = {}
parse_errors = 0

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    parts = line.split("\t")
    if len(parts) < 8:
        parse_errors += 1
        continue
    stream_s, t_s, syn_s, ack_s, src, sport_s, dst, dport_s = parts[:8]

    def as_bool(v):
        v = v.strip().lower()
        if v in ("1", "true"):
            return True
        if v in ("0", "false"):
            return False
        raise ValueError(v)

    try:
        stream = int(stream_s)
        t = float(t_s)
        syn = as_bool(syn_s)
        ack = as_bool(ack_s)
        sport = int(sport_s)
    except ValueError:
        parse_errors += 1
        continue
    if not syn:
        continue

    st = streams.setdefault(
        stream,
        {"syn_t": None, "synack_t": None, "syn_retrans": 0, "sport": None},
    )
    if not ack:
        if st["syn_t"] is None:
            st["syn_t"] = t
            st["sport"] = sport
        else:
            st["syn_retrans"] += 1
    else:
        if st["synack_t"] is None:
            st["synack_t"] = t

rtts_ms = []
unanswered = 0
syn_only = 0
synack_without_syn = 0
retrans_syn_flows = 0

for st in streams.values():
    if st["syn_t"] is None and st["synack_t"] is not None:
        synack_without_syn += 1
        continue
    if st["syn_t"] is None:
        continue
    syn_only += 1
    if st["syn_retrans"]:
        retrans_syn_flows += 1
    if st["synack_t"] is None:
        unanswered += 1
        continue
    dt_ms = (st["synack_t"] - st["syn_t"]) * 1000.0
    if dt_ms < 0:
        continue
    rtts_ms.append(dt_ms)

rtts_ms.sort()
n = len(rtts_ms)


def pct(p):
    if not rtts_ms:
        return float("nan")
    if n == 1:
        return rtts_ms[0]
    idx = min(n - 1, max(0, int(round((p / 100.0) * (n - 1)))))
    return rtts_ms[idx]


answered = n
total_client_syn = syn_only
unans_ratio = (unanswered / total_client_syn * 100.0) if total_client_syn else 0.0

print(f"pcap:              {pcap}")
if host or port:
    h = host or "*"
    p = port or "*"
    print("filter:            host=%s port=%s" % (h, p))
print("tcp.streams(syn):  %d" % len(streams))
print("client SYN flows:  %d" % total_client_syn)
print("answered (SYN-ACK):%d" % answered)
print("unanswered SYN:    %d (%.2f%%)" % (unanswered, unans_ratio))
print("SYN retrans flows: %d" % retrans_syn_flows)
if synack_without_syn:
    print("SYN-ACK w/o SYN:   %d (capture started mid-flow?)" % synack_without_syn)
if parse_errors:
    print("parse skips:       %d" % parse_errors)
print("--- SYN → SYN-ACK RTT (ms) ---")
if answered == 0:
    print("no matched handshake pairs")
else:
    print("n:    %d" % answered)
    print("min:  %.3f" % rtts_ms[0])
    print("p50:  %.3f" % pct(50))
    print("p90:  %.3f" % pct(90))
    print("p99:  %.3f" % pct(99))
    print("max:  %.3f" % rtts_ms[-1])
    print("avg:  %.3f" % (sum(rtts_ms) / answered))
'
