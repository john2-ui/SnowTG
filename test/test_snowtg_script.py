#!/usr/bin/env python3
"""Script authoring, export, argument forwarding and exec handoff; no DPDK needed."""
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
launcher = root / "traffic-gen/snowtg.py"
example = root / "traffic-gen/scenarios/test/phased-http-dns.py"


def run(*args, env=None):
    return subprocess.run([sys.executable, str(launcher), *map(str, args)],
                          text=True, capture_output=True, timeout=10, env=env)


with tempfile.TemporaryDirectory(prefix="snowtg script ") as tmp:
    tmp = Path(tmp)
    exported = tmp / "plan.json"
    env = os.environ.copy()
    env.pop("SNOWTG_PEER", None)
    result = subprocess.run([sys.executable, str(launcher), "--emit-json",
                            str(exported), str(example)], env=env,
                            text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert json.loads(exported.read_text()) == json.loads(example.with_suffix(".json").read_text())

    script = tmp / "custom scenario.py"
    (tmp / "settings.py").write_text("peer = '198.18.0.2'\n")
    script.write_text('''from snowtg import scenario, phase, http, dns
from settings import peer
import sys
assert len(sys.argv) == 1
print("script ran", flush=True)
plan = scenario("loop", concurrency=4,
    phases=(phase(str(i), 1, i * 10) for i in range(1, 4)),
    classes=[http("web", peer, host="test.local"),
             dns("dns", peer, "test.local", qtype="AAAA")])
''')
    fake = tmp / "native generator"
    fake.write_text('#!' + sys.executable + '''
import json, os, sys
with open(sys.argv[-1]) as f:
    plan = json.load(f)
print(json.dumps(dict(plan=plan, args=sys.argv[1:-1], pid=os.getpid())))
sys.exit(17)
''')
    fake.chmod(0o700)
    proc = subprocess.Popen([sys.executable, str(launcher), "--binary", str(fake),
        str(script), "--", "-l", "0,1", "--", "--workers", "1", "--stats-csv",
        "a file;$(literal).csv"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = proc.communicate(timeout=10)
    assert proc.returncode == 17, stderr
    lines = stdout.splitlines()
    assert lines[0] == "script ran", stdout
    captured = json.loads(lines[1])
    assert captured["pid"] == proc.pid
    assert captured["args"] == ["-l", "0,1", "--", "--workers", "1",
                                "--stats-csv", "a file;$(literal).csv"]
    assert [p["target_cps"] for p in captured["plan"]["phases"]] == [10, 20, 30]
    assert captured["plan"]["classes"][0]["http"]["host"] == "test.local"
    assert captured["plan"]["classes"][1]["dns"]["qtype"] == "AAAA"

    result = run("--binary", fake, exported, "--", "-l", "0,1")
    assert result.returncode == 17, result.stderr
    assert json.loads(result.stdout)["args"] == ["-l", "0,1", "--"]
    for source, error in [
        ("x = 1", "export a dict"),
        ("plan = {'x': float('nan')}", "JSON compliant"),
        ("plan = [", "custom scenario.py, line 1"),
        ("from snowtg import scenario\nplan = scenario('x', classes=[], phases=[], cps=1)",
         "use phases OR"),
    ]:
        script.write_text(source)
        result = run("--binary", fake, script)
        assert result.returncode == 1 and error in result.stderr, result
        assert not result.stdout
    script.write_text("from snowtg import scenario\nplan = scenario('fixed', classes=[], duration=2, cps=3)")
    result = run("--emit-json", script, script)
    assert result.returncode == 1 and "overwrite" in result.stderr
    result = run("--emit-json", exported, script)
    assert result.returncode == 0, result.stderr
    assert json.loads(exported.read_text())["target_cps"] == 3

    lua_script = tmp / "custom scenario.lua"
    lua_script.write_text("return {name='x'}")
    missing = run("--emit-json", exported, lua_script,
                  env={**os.environ, "PATH": "", "SNOWTG_LUA": ""})
    assert missing.returncode == 1 and "Lua interpreter missing" in missing.stderr
    if not (os.environ.get("SNOWTG_LUA") or
            any(shutil.which(name) for name in ("lua", "lua5.4", "lua5.3"))):
        print("SKIP: Lua execution checks require lua5.3/lua5.4")
    else:
        result = run("--emit-json", exported, example.with_suffix(".lua"), env=env)
        assert result.returncode == 0, result.stderr
        assert json.loads(exported.read_text()) == json.loads(example.with_suffix(".json").read_text())
        (tmp / "settings.lua").write_text("return {peer='198.18.0.2'}")
        lua_script.write_text('''local tg = require("snowtg")
local peer = require("settings").peer
assert(arg[1] == nil)
print("lua ran")
local phases = {}
for i=1,3 do phases[i] = tg.phase(tostring(i), 1, i*1e3) end
plan = tg.scenario("loop", {concurrency=4, phases=phases,
    classes={tg.http("web", peer, 80, {host="test.local", keepalive=true}),
             tg.dns("dns", peer, "test.local", 53, {qtype="AAAA"})}})
''')
        proc = subprocess.Popen([sys.executable, str(launcher), "--binary", str(fake),
            str(lua_script), "--", "-l", "0,1", "--", "--workers", "1",
            "--stats-csv", "a file;$(literal).csv"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = proc.communicate(timeout=10)
        assert proc.returncode == 17, stderr
        lines = stdout.splitlines()
        assert lines[0] == "lua ran", stdout
        captured = json.loads(lines[1])
        assert captured["pid"] == proc.pid
        assert captured["args"][-1] == "a file;$(literal).csv"
        assert [p["target_cps"] for p in captured["plan"]["phases"]] == [1000, 2000, 3000]
        assert captured["plan"]["classes"][0]["http"]["keepalive"] is True
        assert captured["plan"]["classes"][1]["dns"]["qtype"] == "AAAA"

        lua_script.write_text(r'''local tg = require("snowtg")
local plan = tg.scenario('中文"\\\n\0', {classes={}, duration=1, cps=1})
plan.large = 9223372036854775807
plan.fraction = 1.25
plan.invalid_type = tg.http("web", "198.18.0.2", false, {weight=false})
return plan
''')
        result = run("--emit-json", exported, lua_script)
        assert result.returncode == 0, result.stderr
        data = json.loads(exported.read_text())
        assert data["name"] == '中文"\\\n\0' and data["classes"] == [], data
        assert data["large"] == 9223372036854775807 and data["fraction"] == 1.25
        assert data["invalid_type"]["weight"] is False
        assert data["invalid_type"]["peer"]["port"] is False
        for source, error in [
            ("x=1", "return a table or export 'plan'"),
            ("return {x=0/0}", "non-finite"),
            ("return {x=math.huge}", "non-finite"),
            ("plan={}; plan.x=plan", "cyclic"),
            ("return {phases={[1]=1, [3]=3}}", "sparse"),
            ("return {classes={[1]=1, name='x'}}", "mixed"),
            ("return {callback=function() end}", "unsupported"),
            ("local tg=require('snowtg'); return tg.http('x','1.2.3.4',80,{keepaliv=true})",
             "unknown option"),
            ("local tg=require('snowtg'); return tg.scenario('x',{phases={},cps=1})", "use phases OR"),
            ("return {", "Lua scenario failed"),
            ("os.exit(7)", "exit 7"),
            ("os.exit(0)", "Expecting value"),
        ]:
            previous = exported.read_bytes()
            lua_script.write_text(source)
            result = run("--emit-json", exported, lua_script)
            assert result.returncode == 1 and error in result.stderr, result
            assert exported.read_bytes() == previous
        print("PASS: Lua/Python parity, Lua imports/loops, JSON encoding, failures and native handoff")
print("PASS: Python helpers, sibling imports, export, failures, literal argv and native exec handoff")
