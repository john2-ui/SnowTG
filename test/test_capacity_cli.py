#!/usr/bin/env python3
"""Real net_null capacity CLI: repeated bounds, lower bounds and native failure.

The artificial SLO counts expected HTTP timeouts; this checks orchestration,
not service capacity. No peer/NIC is required.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
from snowtg import scenario, transaction, http_step, assertion

cpus = sorted(c for c in os.sched_getaffinity(0) if c < 128)[:2]
assert len(cpus) == 2, 'requires two allowed CPUs'
base = ['--', '-l', ','.join(map(str, cpus)), '--main-lcore', str(cpus[1]),
        '-m', '256', '--no-huge', '--no-pci', '--vdev=net_null0',
        '--file-prefix=capacity-cli-' + str(os.getpid()), '--', '--workers', '1',
        '--local-ip', '198.18.0.1']
plan = scenario('capacity-cli', duration=1, cps=1, concurrency=32,
    classes=[transaction('http', timeout_ms=30,
        steps=[http_step('get', '198.18.0.2', timeout_ms=20)])],
    assertions=[assertion('error_rate', '==', 1), assertion('error_response_timeout', '<=', 4)])

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    source = root / 'scenario.json'
    source.write_text(json.dumps(plan))
    def run(name, minimum, maximum, expected, *options):
        done = subprocess.run([sys.executable, str(ROOT / 'traffic-gen/snowtg.py'), 'capacity',
            '--output', str(root / name), '--minimum', str(minimum), '--maximum', str(maximum),
            '--precision', '1', '--repeats', '2', *options, str(source), *base],
            capture_output=True, text=True, timeout=90)
        assert done.returncode == expected, (done.stdout, done.stderr)
        data = json.loads((root / name / 'result.json').read_text())
        assert data['exit_code'] == expected
        return data
    measured = run('bracketed', 2, 8, 0)
    assert measured['capacity'] == dict(status='bracketed', passed_cps=4, failed_cps=5), measured
    assert measured['summary']['maximum_sustainable_cps'] == 4
    assert len(measured['rounds']) == 10
    lower = run('lower', 2, 2, 0)
    assert lower['status'] == 'lower_bound_only' and lower['summary']['maximum_sustainable_cps'] is None
    assert lower['summary']['sustainable_cps_lower_bound'] == 2
    below = run('below', 8, 8, 2)
    assert below['status'] == 'below_minimum'
    invalid = run('invalid', 2, 2, 1, '--binary', '/bin/false')
    assert invalid['status'] == 'invalid' and len(invalid['rounds']) == 1
print('PASS: native net_null repeated [4,5) bracket, lower bound, below minimum and invalid abort')
