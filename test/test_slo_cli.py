#!/usr/bin/env python3
"""Real net_null runs: SLO failure/pass, native rejection and failure artifacts."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
cpus = sorted(c for c in os.sched_getaffinity(0) if c < 128)[:3]
if len(cpus) < 3:
    raise SystemExit('requires three allowed CPUs')
owned = tempfile.TemporaryDirectory() if args.output is None else None
out = Path(owned.name) if owned else args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
launcher = ROOT/'traffic-gen/snowtg.py'
base = ['--','-l',','.join(map(str,cpus)), '--main-lcore',str(cpus[2]), '-m','256',
        '--no-huge','--no-pci','--vdev=net_null0','--file-prefix=slo-cli-'+str(os.getpid()),
        '--','--workers','2','--local-ip','198.18.0.1']
script = out/'scenario.py'
script.write_text('''from snowtg import scenario, phase, dns, assertion
plan = scenario('net-null-slo', concurrency=4,
    phases=[phase('warmup',1,20),phase('spike',1,40,start=20),phase('cooldown',1,0)],
    classes=[dns('dns','198.18.0.2','test.invalid')],
    assertions=[assertion('success_rate','>=',.99),
                assertion('latency_ms','<',20,quantile=.99),
                assertion('drained_live_sockets','==',0)])
''')

def run(name, path, expected):
    cmd = [sys.executable, str(launcher),'run','--output',str(out/name),str(path),*base]
    r = subprocess.run(cmd, text=True, capture_output=True, timeout=30)
    (out/(name+'.log')).write_text(r.stdout+r.stderr)
    assert r.returncode == expected, (r.returncode,r.stdout,r.stderr)
    data = json.loads((out/name/'result.json').read_text())
    assert data['exit_code'] == expected
    assert (out/name/'report.html').exists()
    return data

failed = run('failed', script, 2)
assert failed['valid'] and not failed['invalid_reasons'], failed['invalid_reasons']
assert failed['summary']['planned'] == 50 and failed['summary']['skipped'] == 46
assert [a['passed'] for a in failed['assertions']] == [False,False,True]
assert failed['assertions'][1]['actual'] is None
assert failed['timeline'] and all('complete_mean_us' in r for r in failed['timeline'])
assert failed['environment']['dataplane']['device'] == 'net_null0'
assert failed['environment']['worker_lcores'] == cpus[:2]
assert failed['build']['dpdk_version'] and failed['build']['binary_sha256']
assert failed['summary']['maximum_sustainable_cps'] is None
lua = out/'scenario.lua'
lua.write_text('''local tg=require('snowtg')
return tg.scenario('net-null-slo',{concurrency=4,
    phases={tg.phase('warmup',1,20),tg.phase('spike',1,40,{start=20}),tg.phase('cooldown',1,0)},
    classes={tg.dns('dns','198.18.0.2','test.invalid')},
    assertions={tg.assertion('error_rate','==',1),
                tg.assertion('latency_ms','<',7000,{quantile=.99,latency_metric='complete_failure'}),
                tg.assertion('drained_live_sockets','==',0)}})
''')
passed = run('passed', lua, 0)
assert passed['valid'] and all(a['passed'] for a in passed['assertions'])
assert passed['summary']['success'] == 0  # Pass only because this self-test explicitly expects failures.
script.write_text(script.read_text()+"\nplan['load_model']='closed'\n")
invalid = run('invalid', script, 1)
assert not invalid['valid'] and invalid['status'] == 'invalid'
assert 'native process exited nonzero' in invalid['invalid_reasons']
print('PASS: real Python/Lua runs returned 2/0/1 for failed SLO, explicit failure expectation, invalid run')
if owned:
    owned.cleanup()
