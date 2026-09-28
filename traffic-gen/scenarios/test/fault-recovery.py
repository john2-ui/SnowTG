"""Low-rate recovery acceptance against test/workflow_peer.py.

An external controller injects faults during seconds 4..10 after native startup.
The seven-second settling phase exceeds the five-second response deadline.
SNOWTG_EXPECT is error, slow, or success; failures are expected only in the fault
window. A passing negative test does not mean the faulted service met its SLO.
"""
import os
from snowtg import assertion, dns, http, phase, scenario

peer = os.environ.get('SNOWTG_PEER', '192.168.10.234')
protocol = os.environ.get('SNOWTG_PROTOCOL', 'http')
expect = os.environ.get('SNOWTG_EXPECT', 'error')
if protocol not in ('http', 'dns') or expect not in ('error', 'slow', 'success'):
    raise ValueError('invalid SNOWTG_PROTOCOL or SNOWTG_EXPECT')
classes = ([http('http', peer, 18080, keepalive=os.environ.get('SNOWTG_KEEPALIVE', '1') == '1')] if protocol == 'http' else
           [dns('dns', peer, 'snowtg.test', 15353)])
checks = [assertion('success_rate', '==', 1, phase='baseline'),
          assertion('success_rate', '==', 1, phase='recovered'),
          assertion('latency_ms', '<', 20, phase='recovered', quantile=.99),
          assertion('skipped_rate', '==', 0),
          assertion('tx_alloc_fail', '==', 0),
          assertion('drained_live_sockets', '==', 0),
          assertion('tcp_forced_cleanup', '==', 0)]
if expect == 'error':
    checks.append(assertion('error_rate', '>', 0, phase='fault'))
elif expect == 'slow':
    checks.append(assertion('latency_ms', '>=', 100, phase='fault', quantile=.99))
else:
    checks.append(assertion('success_rate', '==', 1, phase='fault'))
plan = scenario('fault-recovery-' + protocol, concurrency=128, report_interval=1,
    phases=[phase('baseline', 4, 10), phase('fault', 6, 10),
            phase('settle', 7, 10), phase('recovered', 8, 10)],
    classes=classes, assertions=checks, seed=20260928,
    purpose='Synthetic fault window and same-process recovery; low-rate correctness test',
    service={'name': 'workflow_peer', 'version': '20260928-fault-fixture'})
