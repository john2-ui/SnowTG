#!/usr/bin/env python3
"""Two-owner timer/branch/dataset accounting without a physical NIC."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'traffic-gen'))
from snowtg import scenario, transaction, http_step, think, branch, check, ref, assertion
# Use two owners with net_null and no huge pages: no NIC binding or peer is needed.
# The combined 20/20 branch split checks that each owner does not restart dataset
# selection at its own successful-admission counter.
cpus=sorted(c for c in os.sched_getaffinity(0) if c<128)[:3]
if len(cpus)<3: raise SystemExit('requires three CPUs')
with tempfile.TemporaryDirectory() as tmp:
    out=Path(tmp)
    plan=scenario('control-workflow',duration=2,cps=20,concurrency=8,seed=17,
        classes=[transaction('business',timeout_ms=1000,dataset=[{'take':True},{'take':False}],steps=[
            think('begin',1),branch('choose',check(ref('data.take'),'==',True),then='yes',otherwise='no'),
            think('yes',minimum=1,maximum=3,next='end'),think('no',2)])],
        assertions=[assertion('success_rate','==',1),assertion('latency_ms','<',100,quantile=.99,step='begin')])
    path=out/'plan.json';path.write_text(json.dumps(plan))
    cmd=[sys.executable,str(ROOT/'traffic-gen/snowtg.py'),'run','--binary',os.environ.get('SNOWTG_TEST_BINARY',str(ROOT/'traffic-gen/build/traffic-gen')),'--output',str(out/'run'),str(path),'--',
         '-l',','.join(map(str,cpus)),'--main-lcore',str(cpus[2]),'-m','256','--no-huge','--no-pci','--vdev=net_null0',
         '--file-prefix=workflow-'+str(os.getpid()),'--','--workers','2','--local-ip','198.18.0.1']
    done=subprocess.run(cmd,text=True,capture_output=True,timeout=30)
    if done.returncode:
        print(done.stdout,done.stderr)
        print((out/'run/traffic-gen.log').read_text())
        print((out/'run/result.json').read_text())
    assert done.returncode==0
    r=json.loads((out/'run/result.json').read_text())
    assert r['summary']['planned']==40 and r['summary']['success']==40
    steps={g['step']:g for g in r['steps']}
    assert steps['yes']['reached']==steps['no']['reached']==20
    assert steps['yes']['branch_skipped']==steps['no']['branch_skipped']==20
    assert r['summary']['request_success_rps']==0 and r['summary']['transaction_success_tps']==20
    assert all(g['not_reached']==0 for g in r['steps'])
    assert r['summary']['drained_live_sockets']==0
    print('PASS: 40 business transactions; deterministic 20/20 branches, timers and two-owner statistics')
    # All fail before opening a socket. A network timeout must not mask a
    # missing-variable or illegal-field serialization failure.
    # Require start_failed counts, not merely eventual network errors: malformed
    # fields must be rejected before opening a socket to the reserved target.
    invalid = scenario('invalid-fields',duration=1,cps=6,concurrency=6,
        classes=[transaction(name,timeout_ms=1000,steps=[http_step('get','192.0.2.1',**fields)])
                 for name,fields in [('control',{'headers':{'X-Test':'bad\x01value'}}),
                                     ('framing',{'headers':{'Expect':'100-continue'}}),
                                     ('missing',{'path':'/${ctx.missing}'})]],
        assertions=[assertion('error_rate','==',1)])
    path.write_text(json.dumps(invalid))
    cmd[cmd.index(str(out/'run'))]=str(out/'invalid')
    done=subprocess.run(cmd,text=True,capture_output=True,timeout=30)
    assert done.returncode==0,(done.stdout,done.stderr)
    r=json.loads((out/'invalid/result.json').read_text())
    assert r['summary']['failed']==6 and r['summary']['drained_live_sockets']==0
    assert sum(s['start_failed'] for s in r['steps'])==6
    assert r['summary']['request_success_rps']==0
    print('PASS: missing variables and illegal/control headers fail before socket creation')
