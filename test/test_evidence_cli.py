#!/usr/bin/env python3
"""Two-worker net_null run with local synthetic external evidence; no SSH/NIC."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
from snowtg import scenario, dns, assertion

cpus = sorted(c for c in os.sched_getaffinity(0) if c < 128)[:3]
assert len(cpus) == 3, 'requires three CPUs'
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    now = time.time_ns()
    (root / 'faults.jsonl').write_text(json.dumps(dict(start_ns=now, end_ns=now + 30_000_000_000,
        category='network', label='synthetic evidence; net_null has no peer')) + '\n')
    (root / 'requests.jsonl').write_text(json.dumps(dict(time_ns=now + 3_000_000_000,
        protocol='dns', duration_ns=200_000_000)) + '\n')
    manifest = dict(schema_version=1, client_host='local', reference_host='local', window_sec=1,
        sources=[dict(kind='host', host='local', role='client', path='host.jsonl'),
                 dict(kind='requests', host='local', path='requests.jsonl'),
                 dict(kind='faults', host='local', path='faults.jsonl')])
    (root / 'manifest.json').write_text(json.dumps(manifest))
    plan = scenario('evidence-cli', duration=1, cps=12, concurrency=2,
        classes=[dns('dns', '198.18.0.2', 'test.invalid')],
        assertions=[assertion('error_rate', '==', 1), assertion('drained_live_sockets', '==', 0)])
    (root / 'plan.json').write_text(json.dumps(plan))
    monitor = subprocess.Popen([sys.executable, str(ROOT / 'traffic-gen/snowtg.py'), 'monitor',
        '--interval', '.1', '--samples', '200', '--output', str(root / 'host.jsonl')])
    try:
        done = subprocess.run([sys.executable, str(ROOT / 'traffic-gen/snowtg.py'), 'run',
            '--evidence', str(root / 'manifest.json'), '--output', str(root / 'run'),
            '--binary', os.environ.get('SNOWTG_TEST_BINARY', str(ROOT / 'traffic-gen/build/traffic-gen')),
            str(root / 'plan.json'), '--', '-l', ','.join(map(str, cpus)), '--main-lcore', str(cpus[2]),
            '-m', '256', '--no-huge', '--no-pci', '--vdev=net_null0', '--file-prefix=evidence-' + str(os.getpid()),
            '--', '--workers', '2', '--local-ip', '198.18.0.1'], capture_output=True, text=True, timeout=40)
        assert done.returncode == 0, (done.stdout, done.stderr)
    finally:
        monitor.terminate()
        monitor.wait(timeout=5)
    result = json.loads((root / 'run/result.json').read_text())
    assert result['valid'] and result['exit_code'] == 0
    assert result['summary']['planned'] == 12 and result['summary']['skipped'] == 10
    assert result['clock_alignment']['status'] == 'bounded', result['clock_alignment']
    assert result['final_counters']['concurrency_blocked_turns'] > 0
    corr = result['correlation']
    assert corr['host_intervals'] and all(s.get('sha256') for s in corr['sources'])
    kinds = {f['category'] for w in corr['windows'] for f in w['findings']}
    assert {'client_concurrency_guard', 'network_signals', 'server_slow', 'unattributed'} <= kinds, kinds
    assert {p['worker'] for p in corr['client_intervals']} == {0, 1}
    assert corr['nic_intervals'] and all('resource_current' in p for p in corr['client_intervals'])
    assert '仍不可归因' in (root / 'run/report.html').read_text()
    assert (root / 'run/evidence/manifest.json').exists()
print('PASS: native clock anchors, two-worker guard, evidence snapshots and automatic JSON/HTML correlation')
