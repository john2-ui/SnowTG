#!/usr/bin/env python3
"""Write a Python/Lua scenario exporting `plan`, then run the native generator.

    python3 traffic-gen/snowtg.py scenario.py -- -l 0,1 -- --workers 1
    python3 traffic-gen/snowtg.py scenario.lua -- -l 0,1 -- --workers 1
    python3 traffic-gen/snowtg.py --emit-json plan.json scenario.py
    python3 traffic-gen/snowtg.py run --output debug/run1 scenario.lua -- <EAL> -- <app>
    python3 traffic-gen/snowtg.py compare baseline/result.json candidate/result.json
    python3 traffic-gen/snowtg.py report run/result.json --output report.html

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


def ref(name):
    """Describe a typed vars/data/ctx lookup without evaluating it in Python."""
    return {"ref": name}


def extract(source, path="", *, index=0):
    """Select a response scalar for native extraction.

    Sources are DNS address or HTTP status/header/json. Header index is zero-based;
    JSON path is a JSON Pointer. Export happens before protocol parser reset.
    """
    return dict(source=source, path=path, index=index)


def check(left, op, right=None):
    """Build a typed comparison; exists intentionally omits the right operand.

    Keep literal types, including False and None. Native execution rejects
    incompatible operand types instead of implicitly converting strings.
    """
    return dict(left=left, op=op, **({} if op == "exists" else {"right": right}))


def transaction(name, *, steps, weight=1, vars=None, dataset=None, timeout_ms=None):
    """Build a weighted business class with ordered native steps.

    One admission covers the entire business, including think time. Dataset paths
    are expanded relative to the script directory before execution; immutable rows
    cycle by global planned ordinal, including arrivals skipped under overload.
    """
    config = dict(steps=list(steps))
    for key, value in (("vars", vars), ("dataset", dataset), ("timeout_ms", timeout_ms)):
        if value is not None:
            config[key] = {"file": str(value)} if key == "dataset" and isinstance(value, (str, Path)) else value
    return dict(name=name, weight=weight, transaction=config)


def http_step(name, ip, port=80, *, path="/", method="GET", host=None,
              keepalive=False, headers=None, body=None, extract=None, checks=None,
              next=None, timeout_ms=None):
    """Build a network step, without independent class weight or admission.

    Templates are resolved against the active native context. keepalive requests
    reuse only when endpoint/Host and response policy permit it. next names a
    later step or end; headers/body cannot override plugin-owned framing.
    """
    step = http(name, ip, port, path=path, method=method, host=host, keepalive=keepalive)
    step.pop("weight"); step.pop("transport"); step["type"] = "http"
    for key, value in (("headers", headers), ("body", body)):
        if value is not None: step["http"][key] = value
    step.update({k: v for k, v in dict(extract=extract, checks=checks, next=next, timeout_ms=timeout_ms).items() if v is not None})
    return step


def dns_step(name, ip, qname, port=53, *, extract=None, checks=None, next=None, timeout_ms=None):
    """Build an A-query step whose exported address may feed a later HTTP target.

    Extraction and checks run on the complete datagram. The native resolver only
    accepts addresses matching the question or an answer-section CNAME chain.
    """
    step = dns(name, ip, qname, port)
    step.pop("weight"); step.pop("transport"); step["type"] = "dns"
    step.update({k: v for k, v in dict(extract=extract, checks=checks, next=next, timeout_ms=timeout_ms).items() if v is not None})
    return step


def think(name, ms=None, *, minimum=None, maximum=None, next=None):
    """Describe fixed milliseconds or an inclusive minimum/maximum range.

    The native compiler rejects conflicting forms. An owner timer schedules the
    continuation; this helper neither sleeps nor samples a Python random number.
    """
    return dict(name=name, type="think", **{k: v for k, v in
        dict(ms=ms, min_ms=minimum, max_ms=maximum, next=next).items() if v is not None})


def branch(name, condition, *, then, otherwise):
    """Build a forward-only conditional jump using a check/ref description.

    otherwise maps to JSON else. Targets name later steps or end; this is not
    a Python callback, and the unselected path does not count as request failure.
    """
    return dict(name=name, type="branch", condition=condition, then=then, **{"else": otherwise})


def assertion(metric, op, value, *, class_name=None, protocol=None, phase=None,
              quantile=None, latency_metric=None, critical=True, step=None):
    """Build an SLO spec; the managed runner validates selectors and thresholds.

    Rates are fractions, latency_ms is milliseconds with quantile in (0,1].
    Latency defaults to successful completion; critical failures set exit code 2.
    """
    result = dict(metric=metric, op=op, value=value, critical=critical)
    result.update({k: v for k, v in dict(
        **{"class": class_name}, protocol=protocol, phase=phase,
        quantile=quantile, latency_metric=latency_metric, step=step).items() if v is not None})
    return result


def scenario(name, *, classes, concurrency=256, phases=None, duration=None,
             cps=None, report_interval=1, assertions=None, purpose=None, service=None, seed=None):
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
    for key, value in (("assertions", assertions), ("purpose", purpose), ("service", service), ("seed", seed)):
        if value is not None:
            result[key] = value
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
    if argv[:1] in (["compare"], ["report"]):
        from snowtg_results import cli
        try:
            return cli(argv)
        except (OSError, ValueError, KeyError, TypeError) as error:
            print(f"snowtg: {error}", file=sys.stderr)
            return 1
    managed = argv[:1] == ["run"]
    if managed:
        argv = argv[1:]
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emit-json", type=Path, metavar="PATH",
                        help="export only; native schema validation happens when run")
    parser.add_argument("--output", type=Path, help="new managed-run directory (CSV, result.json, report.html)")
    parser.add_argument("--baseline", type=Path, help="baseline result.json for the report")
    parser.add_argument("--timeout", type=float, help="managed run wall-clock limit in seconds")
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
        from snowtg_datasets import expand_datasets
        plan = json.loads(text)
        if not isinstance(plan, dict): raise ValueError("scenario must be an object")
        if expand_datasets(plan, path.parent):
            text = json.dumps(plan, ensure_ascii=False, allow_nan=False, indent=2) + "\n"
        if options.emit_json:
            if managed or options.output or options.baseline or options.timeout is not None:
                raise ValueError("--emit-json cannot be combined with managed-run options")
            if options.args:
                raise ValueError("--emit-json does not accept runtime arguments")
            if options.emit_json.resolve() == path:
                raise ValueError("output must not overwrite the input scenario")
            options.emit_json.write_text(text, encoding="utf-8")
            return 0
        plan = json.loads(text)
        if not isinstance(plan, dict):
            raise ValueError("scenario must be an object")
        if managed or options.output or options.baseline or options.timeout is not None or any(
                key in plan for key in ("assertions", "purpose", "service", "dataset_sources")):
            # Managed mode waits for the native child and evaluates final CSVs;
            # scripts still execute only at startup, never in the packet path.
            from snowtg_results import run
            return run(plan, options.binary, options.args, options.output, options.timeout, options.baseline)
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
