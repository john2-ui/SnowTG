#!/usr/bin/env python3
"""Run against workflow_peer.py. Pass an output dir then EAL/app args after --.
Requires a prepared NIC or AF_PACKET; no interface binding is performed here.
"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'traffic-gen'))
from snowtg import assertion
from snowtg_datasets import expand_datasets
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('args',nargs=argparse.REMAINDER)
opts=parser.parse_args();opts.output.mkdir(parents=True,exist_ok=False)
args=opts.args[1:] if opts.args[:1]==['--'] else opts.args
# Reduce the example rate to test behavior deterministically, not peak throughput.
base=json.loads((ROOT/'traffic-gen/scenarios/test/workflow-http-dns.json').read_text())
expand_datasets(base,ROOT/'traffic-gen/scenarios/test')
base.update(duration_sec=1,target_cps=4,max_concurrency=4)


def run(name,plan,success):
    """Execute one expected outcome and require complete resource/accounting drain.

    For negative cases, error_rate == 1 is the expected SLO, so the launcher must
    still exit 0. A harness pass does not mean the business itself succeeded.
    """
    plan['assertions']=[assertion('success_rate' if success else 'error_rate','==',1)]
    path=opts.output/(name+'.json');path.write_text(json.dumps(plan))
    command=[sys.executable,str(ROOT/'traffic-gen/snowtg.py'),'run','--output',str(opts.output/name),str(path),'--',*args]
    done=subprocess.run(command,text=True,capture_output=True,timeout=180)
    (opts.output/(name+'.log')).write_text(done.stdout+done.stderr)
    assert done.returncode==0,(name,done.stdout,done.stderr)
    result=json.loads((opts.output/name/'result.json').read_text())
    assert result['valid'] and result['summary']['planned']==4
    assert result['summary']['success' if success else 'failed']==4
    assert result['summary']['drained_live_sockets']==0
    parents={(g['phase'],g['class']):g['admitted'] for g in result['groups']}
    assert all(g['reached']+g['branch_skipped']+g['not_reached']==parents[(g['phase'],g['class'])] for g in result['steps'])
    print('PASS:',name,flush=True)
    return result

r=run('full',deepcopy(base),True)
assert r['summary']['request_success_rps']==16 and r['summary']['transaction_success_tps']==4
steps={s['step']:s for s in r['steps']}
assert steps['route_a']['success']==steps['route_b']['success']==2
# Change one condition per run so parsing, missing data, capacity, type, and
# timeout failures cannot conceal one another.
for name,mutate,success in [
 ('non-2xx',lambda w:(w['steps'][1]['http'].update(path='/status/404'),w['steps'][1]['checks'][0].update(right=404)),True),
 ('missing-field',lambda w:w['steps'][1]['extract']['token'].update(path='/missing'),False),
 ('unrelated-dns',lambda w:w['steps'][0]['dns'].update(qname='missing.test'),False),
 ('capture-limit',lambda w:w['steps'][1]['http'].update(path='/large'),False),
 ('invalid-json',lambda w:w['steps'][1]['http'].update(path='/invalid'),False),
 ('typed-branch',lambda w:w['steps'][3]['condition'].update(right=True),False),
 ('step-timeout',lambda w:(w['steps'][1]['http'].update(path='/slow'),w['steps'][1].update(timeout_ms=20)),False),
 ('overall-timeout',lambda w:(w['steps'][1]['http'].update(path='/slow'),w.update(timeout_ms=30)),False),
]:
    plan=deepcopy(base);mutate(plan['classes'][0]['transaction']);run(name,plan,success)
# An admitted transaction may finish after its load phase; attribute it to the start.
plan=deepcopy(base);plan.pop('duration_sec');plan.pop('target_cps')
plan['phases']=[dict(name='offer',duration_sec=1,target_cps=4),dict(name='quiet',duration_sec=1,target_cps=0)]
plan['classes'][0]['transaction']['steps'][2]={'name':'pause','type':'think','ms':1200}
r=run('cross-phase',plan,True)
assert sum(g['success'] for g in r['groups'] if g['phase']=='offer')==4
assert sum(g['success'] for g in r['groups'] if g['phase']=='quiet')==0
print('PASS: live workflow success/failure, branches, deadlines, extraction and clean drain')
