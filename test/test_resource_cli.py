#!/usr/bin/env python3
"""Real two-owner net_null runs, with test-only stalled teardown/pool retention."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'traffic-gen'))
from snowtg import scenario, transaction, http_step, assertion

binary = Path(sys.argv[1] if len(sys.argv)>1 else ROOT/'traffic-gen/build/traffic-gen-resource-test').resolve()
cpus = sorted(c for c in os.sched_getaffinity(0) if c<128)[:3]
assert len(cpus)==3
with tempfile.TemporaryDirectory() as tmp:
    out = Path(tmp)
    plan = scenario('resource-drain', duration=1, cps=2, concurrency=2,
        classes=[transaction('http', timeout_ms=30, steps=[http_step('get','198.18.0.2', timeout_ms=20)])],
        assertions=[assertion('error_rate','==',1)])
    path = out/'plan.json'
    path.write_text(json.dumps(plan))
    for fault, expected in ((None,'passed'), ('forced','forced_zero'), ('residual','residual')):
        dest = out/(fault or 'normal')
        env = dict(os.environ)
        env.pop('SNOWTG_RESOURCE_FAULT',None)
        if fault: env['SNOWTG_RESOURCE_FAULT']=fault
        cmd = [sys.executable, str(ROOT/'traffic-gen/snowtg.py'), 'run', '--binary', str(binary),
               '--output', str(dest), str(path), '--', '-l', ','.join(map(str,cpus)),
               '--main-lcore', str(cpus[2]), '-m', '256', '--no-huge', '--no-pci', '--vdev=net_null0',
               '--file-prefix=resource-'+str(os.getpid()), '--', '--workers', '2', '--local-ip', '198.18.0.1']
        done = subprocess.run(cmd, env=env, text=True, capture_output=True, timeout=30)
        result = json.loads((dest/'result.json').read_text())
        evidence = result['resources']
        assert evidence['status']==expected, (fault, evidence, done.stdout, done.stderr)
        assert done.returncode==(1 if fault else 0), (done.stdout,done.stderr)
        assert evidence['complete'] and evidence['trend_complete']
        assert len(evidence['workers'])==2
        for worker in evidence['workers']:
            metrics = worker['metrics']
            assert metrics['flow']['final']['current']==0
            assert metrics['transaction']['final']['current']==0
            assert metrics['workflow']['final']['current']==0
            assert metrics['ready_event']['final']['current']==0
            assert metrics['timer']['final']['current']==0
            if fault=='forced':
                assert worker['forced']
                assert metrics['socket_slot']['final']['before_force']>0
                assert metrics['time_wait']['final']['before_force']>0
                assert all(m['final']['current']==0 for m in metrics.values())
            if fault=='residual':
                assert worker['forced']
                assert metrics['socket_slot']['final']['current']==0
                assert metrics['tcp_tx_chunk']['final']['current']==1
        assert (dest/'report.html').is_file()
        print('PASS:', fault or 'normal', expected, 'two owners; resources observed before destruction')
