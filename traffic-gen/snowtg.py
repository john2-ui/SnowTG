#!/usr/bin/env python3
"""Write a Python/Lua scenario exporting `plan`, then run the native generator.

    python3 traffic-gen/snowtg.py scenario.py -- -l 0,1 -- --workers 1
    python3 traffic-gen/snowtg.py scenario.lua -- -l 0,1 -- --workers 1
    python3 traffic-gen/snowtg.py --emit-json plan.json scenario.py

Scripts may import scenario, phase, http and dns from snowtg. Helpers produce
ordinary dictionaries; Lua scripts use require("snowtg") and may return a plan.
Lua requires lua, lua5.4 or lua5.3 on PATH (or SNOWTG_LUA pointing to it).
The existing C compiler validates the schema at startup.
"""

import argparse
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys


def phase(name, duration, cps, *, start=None):
    """Constant or linear-ramp transaction starts/second; duration is seconds.

    A reused Keep-Alive request consumes one arrival, just like a new connection.
    """
    result = dict(name=name, duration_sec=duration, target_cps=cps)
    if start is not None:
        result["start_cps"] = start
    return result


def http(name, ip, port=80, *, weight=1, path="/", keepalive=False,
         method="GET", host=None):
    """One HTTP traffic class; keepalive=True reuses connections."""
    config = dict(method=method, path=path, keepalive=keepalive)
    if host is not None:
        config["host"] = host
    return dict(name=name, weight=weight, transport="tcp",
                peer=dict(ip=ip, port=port), http=config)


def dns(name, ip, qname, port=53, *, weight=1, qtype="A"):
    """One UDP DNS traffic class (A or AAAA query)."""
    return dict(name=name, weight=weight, transport="udp",
                peer=dict(ip=ip, port=port), dns=dict(qname=qname, qtype=qtype))




def scenario(name, *, classes, concurrency=256, phases=None, duration=None,
             cps=None, report_interval=1):
    """Open arrivals: provide phases OR duration/cps; concurrency is global."""
    result = dict(name=name, load_model="open", max_concurrency=concurrency,
                  report_interval_sec=report_interval, classes=list(classes))
    if phases is not None:
        if duration is not None or cps is not None:
            raise ValueError("use phases OR duration/cps, not both")
        result["phases"] = list(phases)
    else:
        if duration is None or cps is None:
            raise ValueError("provide phases or both duration and cps")
        result.update(duration_sec=duration, target_cps=cps)
    return result


def load_script(path):
    """Execute a trusted script once, with sibling imports and script-only argv."""
    old_path, old_argv = sys.path[:], sys.argv
    try:
        sys.path.insert(0, str(path.parent))
        sys.argv = [str(path)]
        namespace = runpy.run_path(str(path), run_name="__main__")
    finally:
        sys.path[:] = old_path
        sys.argv = old_argv
    plan = namespace.get("plan")
    if not isinstance(plan, dict):
        raise ValueError(f"{path}: script must export a dict named 'plan'")
    return json.dumps(plan, ensure_ascii=False, allow_nan=False, indent=2) + "\n"


def load_lua(path):
    """Execute trusted Lua once; an inherited Linux memfd carries only its plan.

    Script logs remain on stdout/stderr. This executes ordinary user code with
    the launcher's privileges, as load_script does; neither is a sandbox.
    """
    interpreter = os.environ.get("SNOWTG_LUA") or next(
        (found for name in ("lua", "lua5.4", "lua5.3")
         if (found := shutil.which(name))), None)
    if not interpreter:
        raise ValueError("Lua interpreter missing: install lua5.4 or set SNOWTG_LUA")
    # Separate descriptor keeps print()/io.write() logs out of the JSON data.
    with os.fdopen(os.memfd_create("snowtg-lua"), "r+b") as output:
        result = subprocess.run([interpreter, str(Path(__file__).with_suffix(".lua")),
                                 str(path), f"/proc/self/fd/{output.fileno()}"],
                                pass_fds=(output.fileno(),))
        if result.returncode:
            raise ValueError(f"Lua scenario failed (exit {result.returncode}): {path}")
        output.seek(0)
        text = output.read().decode("utf-8")
        if not isinstance(json.loads(text), dict):
            raise ValueError("Lua scenario must produce a plan object")
        return text


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emit-json", type=Path, metavar="PATH",
                        help="export only; native schema validation happens when run")
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().parent / "build/traffic-gen",
                        help="native executable (default: adjacent build/traffic-gen)")
    parser.add_argument("script", type=Path, help="trusted .py/.lua script or existing .json")
    parser.add_argument("args", nargs=argparse.REMAINDER,
                        help="EAL arguments, then -- and traffic-gen arguments")
    options = parser.parse_args(argv)
    try:
        path = options.script.resolve()
        if path.suffix not in (".py", ".lua", ".json"):
            raise ValueError("scenario must be a .py, .lua or .json file")
        if path.suffix == ".lua":
            text = load_lua(path)
        else:
            text = load_script(path) if path.suffix == ".py" else path.read_text()
        if options.emit_json:
            if options.args:
                raise ValueError("--emit-json does not accept runtime arguments")
            if options.emit_json.resolve() == path:
                raise ValueError("output must not overwrite the input scenario")
            options.emit_json.write_text(text, encoding="utf-8")
            return 0
        plan = json.loads(text)
        if not isinstance(plan, dict):
            raise ValueError("scenario must be an object")
        args = options.args
        if args[:1] == ["--"]:
            args = args[1:]
        if "--" not in args:
            args = [*args, "--"]
        # ponytail: Linux memfd lets exec keep the JSON without a temp file or
        # resident Python process; the C loader already supports seekable files.
        fd = os.memfd_create("snowtg-scenario", flags=0)
        try:
            with os.fdopen(os.dup(fd), "w", encoding="utf-8") as output:
                output.write(text)
                output.flush()
                output.seek(0)
            binary = str(options.binary.resolve())
            sys.stdout.flush()
            sys.stderr.flush()
            os.execv(binary, [binary, *args, f"/proc/self/fd/{fd}"])
        finally:
            os.close(fd)
    except (OSError, ValueError, TypeError, SyntaxError, KeyError) as error:
        print(f"snowtg: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
