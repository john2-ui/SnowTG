#!/usr/bin/env python3
"""Run beside retained NUC artifacts as root (perf.data may be mode 600).
Count each sampled period once for the union of timer/expiry stack frames.
"""
import csv,json,re,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1])
rows=[]
# perf may report cpu_core/cycles:u/:; match the header using its first fields.
header=re.compile(r'^\s*(\S+)\s+(\d+)\s+(\d+)\s+.*cycles')
timer=re.compile(r'\b(?:owner_timer_\w+|(?:__)?rte_timer_\w+|wheel_\w+|tg_flow_expire|tg_flow_timeout_\w+)\b')
for directory in sorted(root.iterdir()):
 result_file=directory/'result.json'
 if not result_file.is_file():continue
 result=json.loads(result_file.read_text())
 data=root/(directory.name+'.perf.data')
 samples=root/(directory.name+'.stacks.txt')
 if not samples.exists() or samples.stat().st_size == 0:
  temporary=samples.with_suffix('.tmp')
  with temporary.open('w') as out:
   subprocess.run(['perf','script','-f','-i',str(data),'-F','comm,tid,period,event,ip,sym,dso'],stdout=out,check=True)
  temporary.replace(samples)
 total=timed=count=timer_count=0
 pending=None;tagged=False
 def add():
  global total,timed,count,timer_count
  if pending:
   total+=pending;count+=1
   if tagged:timed+=pending;timer_count+=1
 for line in samples.read_text().splitlines():
  m=header.match(line)
  if m:
   add()
   pending=int(m[3]) if m[1].startswith('dpdk-worker') else None
   tagged=bool(timer.search(line))
  elif timer.search(line):tagged=True
 add()
 variant,protocol,workers,repeat=directory.name.split('-')
 groups=[g for g in result['groups'] if g['phase']=='steady']
 # The perf window is the interior 28 seconds of the 30-second steady phase.
 # Per-request cycles use steady offered/observed success rate, not exact
 # per-packet attribution to sample timestamps.
 success=sum(g.get('success',0) for g in groups)
 rps=success/30
 row=dict(name=directory.name,variant=variant,protocol=protocol,workers=int(workers[1:]),repeat=int(repeat[1:]),
  valid=result['valid'],status=result['status'],summary=result['summary'],
  steady_rps=rps,steady_p99_ms=groups[0]['latency']['complete_success']['p99_us']/1000,
  worker_sample_cycles=total,timer_sample_cycles=timed,worker_samples=count,timer_samples=timer_count,
  timer_percent=100*timed/total if total else None,
  worker_cycles_per_success=total/(rps*28) if rps else None,
  timer_cycles_per_success=timed/(rps*28) if rps else None,
  error_reasons=result['error_reasons'])
 rows.append(row)
(root/'profile-summary.json').write_text(json.dumps(rows,indent=2)+'\n')
print(json.dumps([dict(name=r['name'],timer_pct=r['timer_percent'],timer_samples=r['timer_samples']) for r in rows],indent=2))
