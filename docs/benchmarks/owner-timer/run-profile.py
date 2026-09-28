#!/usr/bin/env python3
"""Run on NUC as root. Existing HP nginx:8888 / DNS:1053 are not modified."""
import json, os, subprocess as sp, sys, time
from pathlib import Path
ROOT=Path('/home/lca/work/snowtg-owner-timer-20260928')
os.environ['LD_LIBRARY_PATH']='/opt/dpdk-26.07-rc3/lib'
os.environ['PKG_CONFIG_PATH']='/opt/dpdk-26.07-rc3/lib/pkgconfig'
mode=sys.argv[1]
out=ROOT/('runs-'+mode);out.mkdir(exist_ok=False)
def cmd(args,check=True):
 r=sp.run(args,text=True,stdout=sp.PIPE,stderr=sp.STDOUT)
 with (out/'commands.jsonl').open('a') as f: f.write(json.dumps(dict(args=args,rc=r.returncode,output=r.stdout))+'\n')
 if check and r.returncode: raise RuntimeError(r.stdout)
 return r.stdout
before=Path('/proc/sys/vm/nr_hugepages').read_text()
(out/'huge.before').write_text(before)
restore=out/'restore.sh'
restore.write_text('#!/bin/bash\nset -eu\n/opt/dpdk-26.07-rc3/bin/dpdk-devbind.py -b igc 0000:64:00.0\nudevadm settle\nsleep 1\nnmcli connection up enp100s0-static\nprintf '+before.strip()+' > /proc/sys/vm/nr_hugepages\n')
unit='snowtg-owner-timer-'+mode
cmd(['systemd-run','--unit='+unit,'--on-active=120m','/bin/bash',str(restore)])
try:
 cmd(['perf','stat','-e','cycles:u','--','true'])
 cmd(['sh','-c','ip -j address; ip -j route; ethtool enp100s0'])
 Path('/proc/sys/vm/nr_hugepages').write_text('1024')
 cmd(['modprobe','vfio-pci'])
 cmd(['ip','link','set','enp100s0','down'])
 cmd(['/opt/dpdk-26.07-rc3/bin/dpdk-devbind.py','--force','-b','vfio-pci','0000:64:00.0'])
 variants=['baseline'] if mode=='pre' else ['baseline','rte','wheel']
 rounds=1 if mode=='pre' else 3
 for repeat in range(rounds):
  order=variants[repeat:]+variants[:repeat]
  for workers,concurrency in [(1,512),(4,2048)]:
   for protocol in ['short','keepalive','dns']:
    for variant in order:
     src=ROOT/variant;sys.path.insert(0,str(src/'traffic-gen'))
     from snowtg import scenario,phase,http,dns
     name=f'{variant}-{protocol}-w{workers}-r{repeat+1}'
     directory=out/name
     cls=dns('dns','192.168.10.234','snowtg.test',1053) if protocol=='dns' else http('http','192.168.10.234',8888,keepalive=protocol=='keepalive')
     rate=100000 if mode=='pre' else {'short':30000,'keepalive':50000,'dns':20000}[protocol]
     plan=scenario(name,concurrency=concurrency,phases=[phase('warmup',10,rate),phase('steady',30,rate)],classes=[cls],report_interval=1)
     path=out/(name+'.json');path.write_text(json.dumps(plan))
     args=['python3',str(src/'traffic-gen/snowtg.py'),'run','--binary',str(src/'traffic-gen/build/traffic-gen'),'--output',str(directory),str(path),'--','-l',','.join(map(str,[8]+[0,2,4,6][:workers])),'--main-lcore','8','-a','0000:64:00.0','-m','1024','--file-prefix','owner-timer-profile','--','--workers',str(workers),'--local-ip','192.168.10.86','--rx-mode','worker','--tx-mode','worker']
     print('START',name,flush=True)
     with (out/(name+'.launch.log')).open('w') as log:
      p=sp.Popen(args,stdout=log,stderr=sp.STDOUT,cwd=src)
      try:
       deadline=time.monotonic()+50
       while time.monotonic()<deadline and p.poll() is None:
        logfile=directory/'traffic-gen.log'
        if logfile.exists() and 'run_metadata ' in logfile.read_text():break
        time.sleep(.1)
       else: raise RuntimeError('no workload epoch: '+name)
       time.sleep(10)
       pid=cmd(['pgrep','-n','-x','traffic-gen']).strip()
       with (out/(name+'.perf.log')).open('w') as perf_log:
        prof=sp.Popen(['perf','record','-e','cycles:u','-F','199','-g','-p',pid,'-o',str(out/(name+'.perf.data')),'--','sleep','28'],stdout=perf_log,stderr=sp.STDOUT)
        assert prof.wait(timeout=40)==0
       rc=p.wait(timeout=160)
       assert rc in (0,2),(name,rc)
       (out/(name+'.perf.txt')).write_text(cmd(['perf','report','--stdio','--no-children','-i',str(out/(name+'.perf.data'))]))
       with (out/(name+'.samples.txt')).open('w') as sample:
        sp.run(['perf','script','-i',str(out/(name+'.perf.data')),'-F','comm,tid,period,event'],stdout=sample,check=True)
       result=json.loads((directory/'result.json').read_text())
       print('DONE',name,result['status'],result['summary'],flush=True)
      finally:
       if p.poll() is None:p.terminate();p.wait(timeout=180)
finally:
 cmd(['bash',str(restore)],check=False)
 cmd(['systemctl','stop',unit+'.timer'],check=False)
 cmd(['sh','-c','ip -br address; cat /proc/sys/vm/nr_hugepages; pgrep -x traffic-gen || true'],check=False)
 cmd(['chown','-R','lca:lca',str(out)],check=False)
