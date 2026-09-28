"""HTTP KA/short/DNS 60/20/20 acceptance; default steady duration is 30 minutes.

Set SNOWTG_PEER, SNOWTG_HTTP_PORT, SNOWTG_DNS_PORT, SNOWTG_CPS,
SNOWTG_DURATION and SNOWTG_SERVICE_VERSION for the prepared test environment.
Shortened runs are smoke checks, not long-duration acceptance evidence.
"""
import os
from snowtg import scenario, phase, http, dns, assertion

peer = os.environ.get('SNOWTG_PEER', '192.168.10.234')
cps = int(os.environ.get('SNOWTG_CPS', '500'))
duration = int(os.environ.get('SNOWTG_DURATION', '1800'))
port = int(os.environ.get('SNOWTG_HTTP_PORT', '8888'))
plan = scenario('mixed-soak', concurrency=256, report_interval=5,
    seed=20260928,
    purpose='Synthetic 60/20/20 HTTP KA/short/DNS stability acceptance; not production traffic',
    service={'name': 'HTTP + DNS', 'version': os.environ.get('SNOWTG_SERVICE_VERSION', 'unspecified')},
    phases=[phase('warmup', 5, max(1, cps // 5)),
            phase('ramp', 5, cps, start=max(1, cps // 5)),
            phase('steady', duration, cps), phase('spike', 5, cps * 2),
            phase('cooldown', 5, max(1, cps // 5))],
    classes=[http('http_keepalive', peer, port, weight=6, keepalive=True),
             http('http_short', peer, port, weight=2),
             dns('dns', peer, 'snowtg.test', int(os.environ.get('SNOWTG_DNS_PORT', '1053')), weight=2)],
    assertions=[assertion('success_rate', '>=', .999),
                assertion('latency_ms', '<', 20, protocol='http', phase='steady', quantile=.99),
                assertion('latency_ms', '<', 10, protocol='dns', phase='steady', quantile=.99),
                assertion('tx_alloc_fail', '==', 0),
                assertion('drained_live_sockets', '==', 0),
                assertion('tcp_forced_cleanup', '==', 0)])
