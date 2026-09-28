#!/usr/bin/env python3
"""Run from the repo root; argv[1] is a new output directory.
Prepared SSH control sockets and source/peer mirrors are required.
The NUC sudo credential stays in memory and is never logged.
"""
import getpass,json,shlex,subprocess as sp,sys,time
from pathlib import Path
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=False)
secret=getpass.getpass('NUC sudo password: ')
assert secret
nr='/home/lca/work/snowtg-owner-timer-20260928'
hr='/home/lc/snowtg-owner-timer-20260928'
hosts={'nuc':('wifi','lca@192.168.21.187'),'hp':('hp','lc@192.168.10.234')}
q=shlex.quote

def ssh(host):
 socket,address=hosts[host]
 return ['ssh','-o','BatchMode=yes','-S','/tmp/snowtg-acceptance-'+socket,address]
def remote(host,command,sudo=False,check=True,timeout=300):
 r=sp.run(ssh(host)+['sudo -S -p "" bash -c '+q(command) if sudo else command],input=secret+'\n' if sudo else None,text=True,capture_output=True,timeout=timeout)
 with (out/'commands.jsonl').open('a') as f:f.write(json.dumps(dict(host=host,command=command,rc=r.returncode,stdout=r.stdout,stderr=r.stderr))+'\n')
 if check and r.returncode:raise RuntimeError(r.stdout+r.stderr)
 return r

def write(host,path,data):
 remote(host,'python3 -c '+q('from pathlib import Path;p=Path('+repr(path)+');t=p.with_suffix(".next");t.write_text('+repr(data)+');t.replace(p)'))
def fault(value):write('hp',hr+'/fault.json',json.dumps(value))

pids=[];prepared=False
try:
 assert not remote('nuc','pgrep -x traffic-gen',check=False).stdout.strip()
 # Retain the exact profiling executables before applying the final workflow
 # deadline integration. The measured legacy workloads never enter this code.
 for backend in ['rte','wheel']:
  remote('nuc',f'mkdir -p {nr}/profile-binaries/{backend}; test -f {nr}/profile-binaries/{backend}/traffic-gen || cp -p {nr}/{backend}/traffic-gen/build/traffic-gen {nr}/{backend}/traffic-gen/build/traffic-gen.build.json {nr}/profile-binaries/{backend}/')
  for filename in ['traffic-gen/core/workflow.c','test/test_workflow_cli.py','test/Makefile',
                   'test/test_tcp_echo.c','apps/tcp-echo/tcp_echo.c','apps/tcp-echo/tcp_echo.h']:
   if not Path(filename).is_file():continue
   sp.run(['scp','-o','ControlPath=/tmp/snowtg-acceptance-wifi',filename,'lca@192.168.21.187:'+nr+'/'+backend+'/'+filename],check=True)
  build=remote('nuc',f'export PKG_CONFIG_PATH=/opt/dpdk-26.07-rc3/lib/pkgconfig LD_LIBRARY_PATH=/opt/dpdk-26.07-rc3/lib CFLAGS="-g -fno-omit-frame-pointer"; cd {nr}/{backend} && make -C traffic-gen -j8 OWNER_TIMER_BACKEND={backend}',timeout=300)
  (out/(backend+'-final-build.log')).write_text(build.stdout+build.stderr)
 remote('nuc','mkdir -p '+nr+'/live-r3')
 restore='#!/bin/bash\nset -eu\n/opt/dpdk-26.07-rc3/bin/dpdk-devbind.py -b igc 0000:64:00.0\nudevadm settle\nsleep 1\nnmcli connection up enp100s0-static\ncat '+nr+'/live-r3/huge.before > /proc/sys/vm/nr_hugepages\n'
 write('nuc',nr+'/live-r3/restore.sh',restore)
 remote('nuc',f'cat /proc/sys/vm/nr_hugepages > {nr}/live-r3/huge.before; systemd-run --unit=snowtg-owner-timer-live --on-active=30m /bin/bash {nr}/live-r3/restore.sh',True)
 prepared=True
 remote('nuc','echo 1024 > /proc/sys/vm/nr_hugepages; modprobe vfio-pci; ip link set enp100s0 down; /opt/dpdk-26.07-rc3/bin/dpdk-devbind.py --force -b vfio-pci 0000:64:00.0',True)
 fault({})
 for script,args in [('workflow_peer.py','--ip 192.168.10.234'),('pressure_peer.py','--ip 192.168.10.234 --fault-file '+hr+'/fault.json')]:
  r=remote('hp',f'(cd {hr} && exec nohup python3 {script} {args}) > {hr}/{script}.log 2>&1 < /dev/null & echo $!')
  # Background shell PID can differ from Python; discover the exact owned command.
  time.sleep(.3)
  r=remote('hp',"pgrep -f '^python3 "+script+" '")
  pids.extend(map(int,r.stdout.split()))
 eal='-l 8,0,2 --main-lcore 8 -a 0000:64:00.0 -m 1024 --file-prefix owner-timer-live -- --workers 2 --local-ip 192.168.10.86 --rx-mode worker --tx-mode worker'
 for backend in ['rte','wheel']:
  src=nr+'/'+backend
  env='export PKG_CONFIG_PATH=/opt/dpdk-26.07-rc3/lib/pkgconfig LD_LIBRARY_PATH=/opt/dpdk-26.07-rc3/lib; '
  # Keep profiling binaries untouched: test builds use their own stack archive.
  r=remote('nuc',env+f'cd {src} && make -C test -j4 BUILD_DIR=build-timer-live OWNER_TIMER_BACKEND={backend}',True,timeout=600)
  (out/(backend+'-unit.log')).write_text(r.stdout+r.stderr)
  r=remote('nuc',env+f'cd {src} && python3 test/test_workflow_live.py {nr}/live-r3/{backend}-workflow -- '+eal,True,timeout=600)
  (out/(backend+'-workflow.log')).write_text(r.stdout+r.stderr)
  r=remote('nuc',env+f'cd {src} && SNOWTG_TEST_BINARY={src}/traffic-gen/build/traffic-gen python3 test/test_workflow_cli.py',True,timeout=120)
  (out/(backend+'-workflow-cli.log')).write_text(r.stdout+r.stderr)
  print('WORKFLOW PASS',backend,flush=True)
  # Use the checked-in scenario helpers locally, matching the remote revision.
  sys.path.insert(0,str(Path.cwd()/'traffic-gen'))
  from snowtg import scenario,phase,http,dns,assertion
  for protocol in ['http','dns']:
   name=backend+'-'+protocol+'-timeout'
   cls=http('http','192.168.10.234',18081,keepalive=True) if protocol=='http' else dns('dns','192.168.10.234','snowtg.test',15354)
   plan=scenario(name,concurrency=256,phases=[phase('normal',3,10),phase('fault',6,10),phase('settle',8,10),phase('recovered',4,10)],classes=[cls],assertions=[assertion('success_rate','==',1,phase='normal'),assertion('success_rate','==',1,phase='recovered'),assertion('error_response_timeout','>',0),assertion('drained_live_sockets','==',0),assertion('tcp_forced_cleanup','==',0)])
   write('nuc',nr+'/live-r3/'+name+'.json',json.dumps(plan))
   command=env+f'cd {src} && python3 traffic-gen/snowtg.py run --output {nr}/live-r3/{name} {nr}/live-r3/{name}.json -- '+eal
   p=sp.Popen(ssh('nuc')+['sudo -S -p "" bash -c '+q(command)],stdin=sp.PIPE,stdout=sp.PIPE,stderr=sp.PIPE,text=True)
   p.stdin.write(secret+'\n');p.stdin.close();p.stdin=None
   try:
    deadline=time.monotonic()+40
    while time.monotonic()<deadline:
     if p.poll() is not None:raise RuntimeError(p.communicate())
     r=remote('nuc',f'grep -q "^run_metadata " {nr}/live-r3/{name}/traffic-gen.log',check=False)
     if r.returncode==0:break
     time.sleep(.1)
    else:raise RuntimeError('missing native epoch')
    time.sleep(3.2)
    fault({'http':'slow','delay_ms':6500} if protocol=='http' else {'dns':'drop'})
    time.sleep(5.5)
    fault({})
    stdout,stderr=p.communicate(timeout=90)
    (out/(name+'.log')).write_text(stdout+stderr)
    assert p.returncode==0,(name,stdout,stderr)
    result=json.loads(remote('nuc',f'cat {nr}/live-r3/{name}/result.json').stdout)
    errors=result['error_reasons']
    assert errors['error_response_timeout']>0
    assert sum(errors.values())==errors['error_response_timeout'],errors
    assert result['summary']['drained_live_sockets']==result['summary']['tcp_forced_cleanup']==0
    (out/(name+'-result.json')).write_text(json.dumps(result,indent=2))
    print('TIMEOUT PASS',name,errors['error_response_timeout'],flush=True)
   finally:
    fault({})
    if p.poll() is None:p.terminate();p.communicate(timeout=180)
finally:
 for pid in pids:remote('hp',f'kill {pid}',check=False)
 if prepared:
  remote('nuc',f'bash {nr}/live-r3/restore.sh; systemctl stop snowtg-owner-timer-live.timer; chown -R lca:lca {nr}/live-r3',True,check=False)
 remote('nuc','ip -br address; cat /proc/sys/vm/nr_hugepages; pgrep -x traffic-gen || true',check=False)
 remote('hp','ss -lntup; curl -s http://192.168.10.234:8888/',check=False)
