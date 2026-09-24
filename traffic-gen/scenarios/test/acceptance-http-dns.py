"""snowtg.py run --output debug/acceptance <this-file> -- <EAL> -- <app>."""
import os
from snowtg import scenario, phase, http, dns, assertion

peer = os.environ.get('SNOWTG_PEER', '192.168.10.234')
plan = scenario('acceptance-http-dns', concurrency=256,
    purpose='HTTP/DNS phased load and SLO acceptance',
    service={'name': 'nginx + dnsmasq', 'version': os.environ.get('SNOWTG_SERVICE_VERSION', 'unspecified')},
    phases=[phase('warmup', 2, 200), phase('ramp', 2, 1000, start=200),
            phase('steady', 4, 1000), phase('cooldown', 2, 200)],
    classes=[http('http_keepalive', peer, 8888, weight=8, keepalive=True),
             dns('dns', peer, 'snowtg.test', 1053, weight=2)],
    assertions=[assertion('success_rate', '>=', .999),
                assertion('latency_ms', '<', 20, class_name='http_keepalive',
                          phase='steady', quantile=.99),
                assertion('tx_alloc_fail', '==', 0),
                assertion('drained_live_sockets', '==', 0)])
