#!/usr/bin/env python3
"""Dataset budgets/types, relative paths, three frontends, and step SLO selectors."""
from copy import deepcopy
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'traffic-gen'))
from snowtg import scenario, transaction, http_step, assertion, check, extract
from snowtg_datasets import expand_datasets
from snowtg_results import validate_assertions, compare

# Check loader bounds and compare complete frontend exports without network I/O.
# Temporary datasets exercise paths resolved relative to the supplied directory.
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp)
    (root/'users.csv').write_text('id,token\n1,"a,b"\n2,"line1\nline2"\n')
    plan=scenario('data',duration=1,cps=1,classes=[transaction('b',dataset='users.csv',steps=[http_step('get','192.0.2.1')])])
    assert expand_datasets(plan,root)
    assert plan['classes'][0]['transaction']['dataset']==[{'id':'1','token':'a,b'},{'id':'2','token':'line1\nline2'}]
    assert len(plan['dataset_sources'][0]['sha256'])==64
    (root/'data.json').write_text('[{"a":true,"n":1,"z":null}]')
    p=deepcopy(plan);p['classes'][0]['transaction']['dataset']={'file':'data.json'}
    expand_datasets(p,root)
    assert p['classes'][0]['transaction']['dataset'][0]==dict(a=True,n=1,z=None)
    (root/'duplicate.json').write_text('[{"token":"first","token":"second"}]')
    p=deepcopy(plan);p['classes'][0]['transaction']['dataset']={'file':'duplicate.json'}
    try:expand_datasets(p,root);raise AssertionError('duplicate JSON key accepted')
    except ValueError:pass
    for text in ('a,a\n1,2\n','a,b\n1\n','a\n1,2\n'):
        (root/'bad.csv').write_text(text)
        p=deepcopy(plan);p['classes'][0]['transaction']['dataset']={'file':'bad.csv'}
        try:expand_datasets(p,root);raise AssertionError(text)
        except ValueError:pass
    for rows in ([],[{'a':[]}],[{'a':'x'*1025}],[{'a':float('nan')}],[{'a':'\0'}],[{'a\0b':'x'}]):
        p=deepcopy(plan);p['classes'][0]['transaction']['dataset']=rows
        try:expand_datasets(p,root);raise AssertionError(rows)
        except ValueError:pass
    p=deepcopy(plan);p['assertions']=[assertion('latency_ms','<',10,quantile=.99,step='get')]
    validate_assertions(p)
    for changes in ({'step':'missing'},{'metric':'drain_ms'},{'latency_metric':'schedule'}):
        bad=deepcopy(p);bad['assertions'][0].update(changes)
        try:validate_assertions(bad);raise AssertionError(changes)
        except ValueError:pass
    outputs=[]
    extensions=['py','json']
    if os.environ.get('SNOWTG_LUA') or any(shutil.which(n) for n in ('lua','lua5.4','lua5.3')): extensions.append('lua')
    for extension in extensions:
        path=root/(extension+'.json')
        env={**os.environ};env.pop('SNOWTG_PEER',None)
        subprocess.run([sys.executable,str(ROOT/'traffic-gen/snowtg.py'),'--emit-json',str(path),
                        str(ROOT/('traffic-gen/scenarios/test/workflow-http-dns.'+extension))],check=True,env=env)
        outputs.append(json.loads(path.read_text()))
    assert all(p==outputs[0] for p in outputs)
    # Even equal workload hashes must not override explicit digest/unit mismatches.
    result=dict(schema_version=2,valid=True,scenario=outputs[0],environment={},
                workload_sha256='same',summary={},metric_units={'success_rps':'transactions/second'})
    changed=deepcopy(result)
    changed['scenario']['dataset_sources'][0]['sha256']='different'
    assert 'dataset source digests differ' in compare(result,changed)['reasons']
    changed=deepcopy(result);changed['metric_units']['success_rps']='requests/second'
    assert 'statistical units differ' in compare(result,changed)['reasons']
print('PASS: CSV/JSON datasets, selectors and Python/Lua/JSON workflow parity')
