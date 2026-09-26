"""Managed runs, SLO evaluation, result comparison and offline HTML; stdlib only.

This control process waits for native completion before reading CSVs. Base
groups are disjoint phase/class populations already merged across workers;
all wider percentiles are recomputed from bins, never averaged. Measurement
validity, SLO acceptance and baseline comparability are separate decisions.
"""
import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
from decimal import Decimal, ROUND_CEILING
import hashlib
import html
import json
import math
import operator
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import uuid

# Version 2 adds step groups and explicit TPS/RPS units; the loader still accepts v1.
SCHEMA = 2
OPS = {"<": operator.lt, "<=": operator.le, "==": operator.eq,
       ">=": operator.ge, ">": operator.gt, "!=": operator.ne}
LATENCIES = {"schedule", "connect", "first_rx", "complete", "scheduled_complete",
             "complete_success", "complete_failure"}
COUNTS = ("planned", "attempted", "skipped", "admitted", "success", "failed", "start_failed")
SCOPED = {"success_rate", "admitted_success_rate", "error_rate", "skipped_rate",
          "success_rps", "latency_ms"}
GLOBAL = {"tx_alloc_fail", "drained_live_sockets", "tcp_forced_cleanup", "drain_ms"}
EXTRA = {"assertions", "purpose", "service", "dataset_sources"}
LIMITS = ["Latency is client-observed software timing, not NIC timestamps or server-only time.",
          "Histogram quantiles use bucket upper bounds (up to ~6.25% plus microsecond rounding).",
          "Success rate uses planned arrivals; skipped/start-failed requests have no latency sample.",
          "Timeline latency is interval mean terminal latency, not interval P99; error rate also includes non-resource start failures; worker samples are asynchronous.",
          "Success RPS is completed successes attributed to planned phases / offered-load duration; it includes completions during drain.",
          "A single run does not determine maximum sustainable load (requires the later load-search feature)."]


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def write_json(path, value):
    """Replace a complete JSON document on the same filesystem (no fsync promise)."""
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def phases(plan):
    return plan.get("phases") or [dict(name="steady", duration_sec=plan["duration_sec"],
                                       target_cps=plan["target_cps"])]


def validate_assertions(plan):
    """Validate launcher-only metadata; native C still validates the workload.

    Selectors must name existing populations, and run-wide resource counters
    cannot take selectors. This runs before creating the output directory.
    """
    assertions = plan.get("assertions", [])
    if not isinstance(assertions, list):
        raise ValueError("assertions must be an array")
    classes = {c["name"] for c in plan["classes"]}
    protocols = {"transaction" if "transaction" in c else "http" if "http" in c else "dns" for c in plan["classes"]}
    steps = [(c["name"], step) for c in plan["classes"] for step in c.get("transaction", {}).get("steps", [])]
    phase_names = {p["name"] for p in phases(plan)}
    for item in assertions:
        if not isinstance(item, dict) or set(item) - {
                "metric", "op", "value", "class", "protocol", "phase", "quantile", "latency_metric", "critical", "step"}:
            raise ValueError("invalid assertion fields")
        metric, value = item.get("metric"), item.get("value")
        if metric not in SCOPED | GLOBAL or item.get("op") not in OPS:
            raise ValueError("unsupported assertion metric/operator")
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError("assertion value must be finite numeric")
        if type(item.get("critical", True)) is not bool:
            raise ValueError("critical must be boolean")
        scoped_protocols = {step["type"] for cls, step in steps if step["name"] == item["step"] and ("class" not in item or item["class"] == cls)} if "step" in item else protocols
        if "step" in item and (metric not in SCOPED or not scoped_protocols):
            raise ValueError("invalid assertion step")
        for key, names in (("class", classes), ("protocol", scoped_protocols), ("phase", phase_names)):
            if key in item and (metric not in SCOPED or item[key] not in names):
                raise ValueError("invalid assertion selector: " + key)
        if "class" in item and "protocol" in item and "step" not in item:
            cls = next(c for c in plan["classes"] if c["name"] == item["class"])
            if item["protocol"] not in cls:
                raise ValueError("class/protocol selectors do not match")
        if metric == "latency_ms":
            q = item.get("quantile")
            if type(q) not in (int, float) or not math.isfinite(q) or not 0 < q <= 1:
                raise ValueError("latency quantile must be in (0,1]")
            if item.get("latency_metric", "complete_success") not in ({"complete", "complete_success", "complete_failure"} if "step" in item else LATENCIES):
                raise ValueError("unknown latency_metric")
        elif "quantile" in item or "latency_metric" in item:
            raise ValueError("quantile/latency_metric apply only to latency_ms")
    if "purpose" in plan and not isinstance(plan["purpose"], str):
        raise ValueError("purpose must be a string")
    if "service" in plan and (not isinstance(plan["service"], dict) or any(
            not isinstance(k, str) or not isinstance(v, str) for k, v in plan["service"].items())):
        raise ValueError("service metadata must contain string keys/values")
    return assertions


def histogram(rows):
    """Merge disjoint populations' upper-bound bins, retaining the observed max."""
    bins = Counter()
    maximum = samples = 0
    for row in rows:
        for upper, count in row["buckets"]:
            bins[upper] += count
        samples += row["samples"]
        maximum = max(maximum, row["max_us"])
    return dict(samples=samples, max_us=maximum, buckets=sorted(bins.items()))


def quantile(hist, q):
    """Nearest-rank upper bound in us; None means no samples, not zero latency.

    Decimal avoids rounding a rank just above an integer due to binary floats.
    Callers supply a validated q in (0,1]; cap the bucket bound at observed max.
    """
    if not hist["samples"]:
        return None
    rank = int((Decimal(str(q)) * hist["samples"]).to_integral_value(rounding=ROUND_CEILING))
    count = 0
    for upper, size in hist["buckets"]:
        count += size
        if count >= rank:
            return min(upper, hist["max_us"])
    raise ValueError("histogram count mismatch")


def distribution(hist):
    return dict(hist, **{name: quantile(hist, q) for name, q in (
        ("p50_us", .5), ("p90_us", .9), ("p95_us", .95), ("p99_us", .99), ("p999_us", .999))})


def selected(result, spec):
    """Select either business groups or step groups, never both.

    Omitting step preserves legacy class-level SLO semantics; mixing the two
    populations would count the same business more than once.
    """
    return [g for g in result.get("steps" if "step" in spec else "groups", []) if all(
        key not in spec or spec[key] == g[key] for key in ("phase", "class", "protocol", "step"))]


def measure(result, spec):
    """Measure selected phase/class groups using planned arrivals as denominator.

    Only admitted_success_rate uses admissions. Skipped/start-failed arrivals
    count against success but have no latency samples. Successful RPS includes
    drain completions attributed to their planned phase / offered-load seconds.
    """
    metric = spec["metric"]
    if metric in GLOBAL:
        return result["summary"].get(metric)
    groups = selected(result, spec)
    if not groups:
        return None
    counts = {k: sum(g[k] for g in groups) for k in COUNTS}
    if metric == "latency_ms":
        hist = histogram([g["latency"][spec.get("latency_metric", "complete_success")] for g in groups])
        value = quantile(hist, spec["quantile"])
        return None if value is None else value / 1000
    if metric == "success_rps":
        seconds = sum(p["duration_sec"] for p in phases(result["scenario"])
                      if "phase" not in spec or spec["phase"] == p["name"])
        return counts["success"] / seconds if seconds else None
    denominator = counts["admitted"] if metric == "admitted_success_rate" else counts["planned"]
    numerator = {"success_rate": counts["success"], "admitted_success_rate": counts["success"],
                 "error_rate": counts["planned"] - counts["success"], "skipped_rate": counts["skipped"]}[metric]
    return numerator / denominator if denominator else None


def evaluate(result):
    """Missing samples fail even a zero threshold; critical affects exit status."""
    checks = []
    for spec in result["scenario"].get("assertions", []):
        actual = measure(result, spec)
        passed = actual is not None and OPS[spec["op"]](actual, spec["value"])
        checks.append(dict(spec, actual=actual, passed=passed,
                           reason=None if passed else ("no samples" if actual is None else "threshold not met")))
    return checks


def read_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def environment():
    cpu = read_text("/proc/cpuinfo") or ""
    model = re.search(r"^model name\s*:\s*(.*)$", cpu, re.M)
    return dict(host=os.uname().nodename, kernel=os.uname().release,
        cpu_model=model.group(1) if model else None, logical_cpus=os.cpu_count(),
        allowed_cpus=sorted(os.sched_getaffinity(0)), memory=read_text("/proc/meminfo"),
        machine=read_text("/sys/class/dmi/id/product_name"),
        numa={p.name: read_text(p / "cpulist") for p in Path("/sys/devices/system/node").glob("node[0-9]*")})


def read_csv(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows or any(None in row or any(v is None for v in row.values()) for row in rows):
        raise ValueError("missing/truncated CSV: " + path.name)
    return rows


def collect(result, output):
    """Populate a result from final native CSVs, recording measurement defects.

    Missing/malformed populations raise; counter disagreements, failed drain
    or link/counter discontinuities append invalid_reasons. Call after the
    child exits: partial files must never look like a successful measurement.
    """
    stats, nic, latency = (read_csv(output / name) for name in ("workers.csv", "main.csv", "latency.csv"))
    invalid = result["invalid_reasons"]
    finals = [r for r in stats if r["scope"] == "aggregate" and r["phase"] == "final"]
    if len(finals) != 1 or nic[-1]["phase"] != "final":
        raise ValueError("missing/duplicate final statistics")
    final = {k: int(v) for k, v in finals[0].items() if k not in ("scope", "phase")}
    result["final_counters"] = final
    for field in ("active", "live_sockets", "tcp_forced_cleanup", "tcp_pool_objects_in_use", "stats_queue_drops"):
        if final[field]:
            invalid.append("nonzero final " + field)
    groups = {}
    drain = None
    for row in latency:
        if row["scope"] == "run" and row["metric"] == "drain":
            drain = int(row["max_us"]) / 1000 if int(row["samples"]) == 1 else None
        # Worker and protocol rows overlap these merged class rows. Reading
        # all scopes would count each transaction multiple times.
        if row["scope"] != "class":
            continue
        key = (row["load_phase"], row["class"], row["protocol"])
        g = groups.setdefault(key, dict(phase=key[0], **{"class": key[1]}, protocol=key[2],
                                        latency={}, **{k: int(row[k]) for k in COUNTS}))
        if any(g[k] != int(row[k]) for k in COUNTS) or row["metric"] in g["latency"]:
            raise ValueError("inconsistent/duplicate latency group")
        bins = [list(map(int, pair.split(":"))) for pair in row["buckets"].split(";") if pair]
        samples, maximum = int(row["samples"]), int(row["max_us"])
        if sum(n for _, n in bins) != samples or any(n < 0 for _, n in bins):
            raise ValueError("invalid histogram count")
        g["latency"][row["metric"]] = distribution(dict(samples=samples, max_us=maximum, buckets=bins))
    result["groups"] = list(groups.values())
    expected = {(p["name"], c["name"]) for p in phases(result["scenario"]) for c in result["scenario"]["classes"]}
    if {(g["phase"], g["class"]) for g in groups.values()} != expected:
        raise ValueError("missing/unexpected phase/class groups")
    for g in groups.values():
        if (set(g["latency"]) != LATENCIES or g["planned"] != g["attempted"] + g["skipped"] or
            g["attempted"] != g["admitted"] + g["start_failed"] or g["admitted"] != g["success"] + g["failed"] or
            g["latency"]["complete"]["samples"] != g["admitted"] or
            g["latency"]["complete_success"]["samples"] != g["success"]):
            raise ValueError("request/histogram accounting mismatch")
    summary = {k: sum(g[k] for g in groups.values()) for k in COUNTS}
    for own, native in (("planned", "arrivals_planned"), ("skipped", "arrivals_skipped"), ("success", "success")):
        if summary[own] != final[native]:
            invalid.append("CSV totals disagree: " + own)
    expected_arrivals = sum(math.ceil(p["duration_sec"] * (p.get("start_cps", p["target_cps"]) + p["target_cps"]) / 2)
                            for p in phases(result["scenario"]))
    if summary["planned"] != expected_arrivals:
        invalid.append("planned arrival count does not cover the full scenario")
    if drain is None:
        invalid.append("clean drain not observed")
    summary.update(tx_alloc_fail=final["tx_alloc_fail"], drained_live_sockets=final["live_sockets"],
                   tcp_forced_cleanup=final["tcp_forced_cleanup"], drain_ms=drain,
                   duration_sec=sum(p["duration_sec"] for p in phases(result["scenario"])),
                   maximum_sustainable_cps=None)
    result["summary"] = summary
    for metric in ("success_rate", "admitted_success_rate", "error_rate", "skipped_rate", "success_rps"):
        summary[metric] = measure(result, dict(metric=metric))
    for name in ("p95_ms", "p99_ms"):
        summary[name] = measure(result, dict(metric="latency_ms", quantile=.95 if name == "p95_ms" else .99))
    result["latency"] = {metric: distribution(histogram([g["latency"][metric] for g in groups.values()]))
                         for metric in sorted(LATENCIES)}
    # Keep step rows separate from class-level business totals and their histograms.
    result["steps"] = []
    step_groups = {}
    for row in latency:
        if row["scope"] != "step": continue
        key = (row["load_phase"], row["class"], row["step"])
        counters = {k: int(row[k]) for k in COUNTS}
        counters.update(reached=int(row["reached"]), started=int(row["started"]),
                        branch_skipped=int(row["branch_skipped"]), not_reached=int(row["not_reached"]))
        group = step_groups.setdefault(key, dict(phase=key[0], **{"class": key[1]}, step=key[2],
                                                 protocol=row["protocol"], latency={}, **counters))
        if any(group[k] != v for k, v in counters.items()) or row["metric"] in group["latency"]:
            raise ValueError("duplicate/inconsistent step counters")
        bins = [list(map(int, pair.split(":"))) for pair in row["buckets"].split(";") if pair]
        samples = int(row["samples"])
        if sum(n for _, n in bins) != samples or any(n < 0 for _, n in bins):
            raise ValueError("invalid step histogram")
        group["latency"][row["metric"]] = distribution(dict(samples=samples, max_us=int(row["max_us"]), buckets=bins))
    expected_steps = {(p["name"], c["name"], step["name"])
                      for p in phases(result["scenario"]) for c in result["scenario"]["classes"]
                      for step in c.get("transaction", {}).get("steps", [])}
    if set(step_groups) != expected_steps:
        raise ValueError("missing/unexpected workflow step groups")
    # Every admitted business must reach, bypass, or never reach each step.
    # CSV failed excludes start_failed; complete step samples include preparation
    # failures, so their population is reached rather than started.
    parents = {(g["phase"], g["class"]): g for g in groups.values()}
    for g in step_groups.values():
        if (set(g["latency"]) != {"complete", "complete_success", "complete_failure"} or
            g["reached"] != g["started"] + g["start_failed"] or g["started"] != g["success"] + g["failed"] or
            g["reached"] + g["branch_skipped"] + g["not_reached"] != parents[(g["phase"], g["class"])]["admitted"] or
            g["latency"]["complete"]["samples"] != g["reached"] or
            g["latency"]["complete_success"]["samples"] != g["success"] or
            g["latency"]["complete_failure"]["samples"] != g["failed"] + g["start_failed"]):
            raise ValueError("step accounting mismatch")
    result["steps"] = list(step_groups.values())
    # Retain success_rps as the legacy class-completion alias. Network RPS counts
    # only successful HTTP/DNS exchanges; think/branch successes are not requests.
    summary["transaction_success_tps"] = summary["success_rps"]
    summary["request_success_rps"] = (sum(g["success"] for g in result["groups"] if g["protocol"] != "transaction") +
        sum(g["success"] for g in result["steps"] if g["protocol"] in ("http", "dns"))) / summary["duration_sec"]
    result["metric_units"] = dict(transaction_success_tps="business transactions/second", request_success_rps="network steps/second",
        success_rps="legacy alias: completed class-level work/second")
    result["dataset_sources"] = result["scenario"].get("dataset_sources", [])
    log = (output / "traffic-gen.log").read_text(errors="replace")
    meta = {}
    for line in log.splitlines():
        if line.startswith(("dataplane driver=", "run_metadata ")):
            meta.update(dict(re.findall(r"(\w+)=([^\s]+)", line)))
    if "epoch_cycles" not in meta or "timer_hz" not in meta:
        invalid.append("missing native clock metadata; rebuild traffic-gen")
    result["workflow_memory"] = [dict(worker=int(w), reserved_bytes=int(n), capacity=int(c))
        for w,n,c in re.findall(r"workflow_memory worker=(\d+) bytes=(\d+) capacity=(\d+)", log)]
    result["environment"]["dataplane"] = meta
    result["environment"]["worker_lcores"] = sorted(int(r["lcore"]) for r in stats
        if r["scope"] == "worker" and r["phase"] == "final")
    epoch = int(meta.get("epoch_cycles", 0)) / int(meta.get("timer_hz", 1)) * 1e6
    hz = int(meta.get("timer_hz", 1))
    # Each worker has its own cumulative sample history. Difference within
    # that history, not adjacent CSV rows; interval means are not interval P99.
    previous, timeline = {}, []
    for row in stats:
        if row["scope"] != "worker":
            continue
        r = {k: int(v) for k, v in row.items() if k not in ("scope", "phase")}
        old = previous.get(r["worker"], dict(timestamp_us=epoch, success=0, done=0, fail=0,
                                             complete_samples=0, complete_sum_cycles=0))
        dt = (r["timestamp_us"] - old["timestamp_us"]) / 1e6
        ds, dd, df = (r[k] - old[k] for k in ("success", "done", "fail"))
        ns = r["complete_samples"] - old["complete_samples"]
        dc = r["complete_sum_cycles"] - old["complete_sum_cycles"]
        if min(ds, dd, df, ns, dc) < 0:
            invalid.append("non-monotonic worker counters")
        if dt > 0:
            timeline.append(dict(worker=r["worker"], time_sec=(r["timestamp_us"] - epoch) / 1e6,
                interval_sec=dt, success_rps=ds / dt, completed_error_rate=df / dd if dd else None,
                complete_mean_us=dc * 1e6 / hz / ns if ns else None,
                active=r["active"], live_sockets=r["live_sockets"], tx_peak=r["tx_peak"],
                payload_peak=r["payload_peak"], load_phase_index=r["load_phase_index"]))
        previous[r["worker"]] = r
    result["timeline"] = timeline
    peaks = ("tx_peak", "payload_peak", "ofo_segments_peak", "ofo_bytes_peak", "dirty_tx_high_water", "ring_hwm_in", "ring_hwm_out")
    result["resource_peaks"] = {k: dict(value=final[k], scope="maximum per-worker high-water mark; not simultaneous total") for k in peaks}
    result["resource_peaks"]["active_sampled"] = dict(value=max((r["active"] for r in timeline), default=0), scope="max sampled single worker")
    error_fields = ("fail_connect", "fail_io", "fail_proto", "fail_resource", "tx_alloc_fail", "rx_ring_drops",
                    "tx_nic_drops", "udp_tx_queue_drops", "rx_handoff_drops", "tcp_forced_cleanup")
    result["errors"] = {k: final[k] for k in error_fields}
    result["errors"].update(start_failed=summary["start_failed"], skipped=summary["skipped"])
    result["nic_samples"] = [{k: v if k == "phase" else int(v) for k, v in r.items()} for r in nic]
    up = [r for r in result["nic_samples"] if r.get("link_rc") == 0 and r.get("link_up") == 1]
    speeds = {r["link_mbps"] for r in up}
    result["environment"]["link_mbps"] = sorted(speeds)
    if any(r.get("link_rc") != 0 or not r.get("link_up") for r in result["nic_samples"]):
        invalid.append("link down or link status unavailable during measurement")
    if len(speeds) > 1:
        invalid.append("link speed changed during measurement")
    if any(not r["nic_delta_valid"] for r in result["nic_samples"]):
        invalid.append("NIC counters unavailable/reset during measurement")
    result["aggregates"] = []
    for keys in (("phase",), ("protocol",), ("phase", "protocol")):
        for values in sorted({tuple(g[k] for k in keys) for g in groups.values()}):
            spec = dict(zip(keys, values))
            subset = selected(result, spec)
            result["aggregates"].append(dict(selectors=spec,
                counts={k: sum(g[k] for g in subset) for k in COUNTS},
                success_rps=measure(result, dict(spec, metric="success_rps")),
                latency={m: distribution(histogram([g["latency"][m] for g in subset])) for m in sorted(LATENCIES)}))


def run(plan, binary, args, output=None, timeout=None, baseline=None):
    """Run once in a fresh artifact directory: 0 passed, 2 critical SLO, 1 invalid.

    The native child owns packet work. Timeout/interrupt terminates its process
    group and retains failure artifacts. --baseline adds an advisory comparison
    to the report; it does not change this run's SLO-based exit code.
    """
    validate_assertions(plan)
    baseline_data = load_result(baseline) if baseline else None
    output = Path(output or ("debug/snowtg-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:6])).resolve()
    if args[:1] == ["--"]:
        args = args[1:]
    if "--" not in args:
        args = [*args, "--"]
    split = args.index("--")
    reserved = ("--stats-csv", "--latency-csv", "--dataplane-csv")
    if any(a.split("=")[0] in reserved for a in args[split + 1:]):
        raise ValueError("managed runs own CSV paths; use --output instead")
    duration = sum(p["duration_sec"] for p in phases(plan))
    if timeout is not None and (not math.isfinite(timeout) or timeout <= 0):
        raise ValueError("timeout must be positive and finite")
    output.mkdir(parents=True, exist_ok=False)
    runtime = {k: v for k, v in plan.items() if k not in EXTRA}
    write_json(output / "scenario.json", plan)
    write_json(output / "runtime.json", runtime)
    command = [str(binary.resolve()), *args, "--stats-csv", str(output / "workers.csv"),
               "--latency-csv", str(output / "latency.csv"), "--dataplane-csv", str(output / "main.csv"),
               str(output / "runtime.json")]
    result = dict(schema_version=SCHEMA, started_utc=datetime.now(timezone.utc).isoformat(),
                  scenario=plan, scenario_sha256=digest(plan), workload_sha256=digest(runtime),
                  purpose=plan.get("purpose", plan.get("name")), service=plan.get("service", {}),
                  service_metadata_source="user-declared" if plan.get("service") else "unavailable",
                  command=command, runtime_arguments=args, environment=environment(),
                  build={}, invalid_reasons=[], limitations=LIMITS[:], summary={}, groups=[],
                  assertions=[], valid=False, exit_code=1,
                  controller_sha256=digest({p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (Path(__file__), Path(__file__).with_name("snowtg.py"), Path(__file__).with_name("snowtg.lua"), Path(__file__).with_name("snowtg_datasets.py"))}))
    # Stopping offered load does not stop admitted businesses. Match the native
    # default deadline, then retain time for socket drain before killing the child.
    business_timeout = max((c["transaction"].get("timeout_ms", sum(
        step.get("timeout_ms", 5000) if step["type"] in ("http", "dns") else step.get("max_ms", step.get("ms", 0))
        for step in c["transaction"]["steps"])) / 1000
        for c in plan["classes"] if "transaction" in c), default=0)
    start = time.monotonic()
    try:
        sha = hashlib.sha256(binary.read_bytes()).hexdigest()
        sidecar = binary.with_suffix(".build.json")
        result["build"] = json.loads(sidecar.read_text()) if sidecar.exists() else {}
        if result["build"].get("binary_sha256") != sha:
            result["build"] = dict(binary_sha256=sha, metadata_status="missing or hash mismatch")
            result["limitations"].append("Build provenance unavailable; rebuild binary for commit/compiler metadata.")
        if not result["service"]:
            result["limitations"].append("Service version/configuration not supplied.")
        with (output / "traffic-gen.log").open("w") as log:
            proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                result["native_exit_code"] = proc.wait(timeout=timeout or duration + (business_timeout + 150 if business_timeout else 90))
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                result["native_exit_code"] = proc.returncode
                result["invalid_reasons"].append("interrupted" if isinstance(error, KeyboardInterrupt) else "run timeout")
        if result["native_exit_code"] != 0:
            result["invalid_reasons"].append("native process exited nonzero")
        collect(result, output)
        result["assertions"] = evaluate(result)
    except (OSError, ValueError, KeyError, TypeError, ZeroDivisionError) as error:
        result["invalid_reasons"].append(str(error))
        result["assertions"] = [dict(a, actual=None, passed=False, reason="measurement invalid")
                                for a in plan.get("assertions", [])]
    result["wall_time_sec"] = time.monotonic() - start
    result["valid"] = not result["invalid_reasons"]
    failed = any(not a["passed"] and a.get("critical", True) for a in result["assertions"])
    result["exit_code"] = 1 if not result["valid"] else 2 if failed else 0
    result["status"] = "invalid" if not result["valid"] else "failed" if failed else "passed"
    if baseline_data:
        result["comparison"] = compare(baseline_data, result)
    write_json(output / "result.json", result)
    (output / "report.html").write_text(report(result), encoding="utf-8")
    for a in result["assertions"]:
        print(f"{'PASS' if a['passed'] else 'FAIL'} {a['metric']}: {a['actual']} {a['op']} {a['value']}")
    print(f"{result['status']}: {output / 'result.json'} (exit {result['exit_code']})")
    return result["exit_code"]


def load_result(path):
    data = json.loads(Path(path).read_text())
    if not isinstance(data, dict) or data.get("schema_version") not in (1, SCHEMA):
        raise ValueError("unsupported result schema")
    if any(not isinstance(data.get(k), dict) for k in ("scenario", "summary", "environment")):
        raise ValueError("incomplete result object")
    return data


def change(old, new):
    """Report absolute/relative change; a nonzero change from zero has no percent."""
    if old is None or new is None:
        return dict(baseline=old, candidate=new, delta=None, percent=None, reason="not measured")
    return dict(baseline=old, candidate=new, delta=new - old,
                percent=(new - old) / old * 100 if old else (0 if new == 0 else None),
                reason="zero baseline" if old == 0 and new else None)


def compare(baseline, candidate, tolerance=5.0):
    """Gate a single-run comparison on equal workloads, SLOs and tested setup.

    Ignore only EAL file-prefix among runtime arguments. RPS/latency use a
    relative tolerance; any error-rate increase/new error type blocks acceptance.
    Resource peaks are reported, not gated. Missing samples or critical SLOs
    make the recommendation inconclusive; this is not a capacity search.
    """
    reasons = []
    if not baseline.get("valid") or not candidate.get("valid"):
        reasons.append("one or both runs invalid")
    if baseline.get("schema_version", 1) != candidate.get("schema_version", 1) and any(
            "transaction" in c for r in (baseline,candidate) for c in r.get("scenario", {}).get("classes", [])):
        reasons.append("workflow metric schema differs")
    if baseline.get("workload_sha256") != candidate.get("workload_sha256"):
        reasons.append("workloads differ")
    # File locations may differ across machines; content digests must agree.
    def dataset_digests(result):
        return [s["sha256"] for s in result.get("scenario", {}).get("dataset_sources", [])]
    if dataset_digests(baseline) != dataset_digests(candidate):
        reasons.append("dataset source digests differ")
    if baseline.get("schema_version", 1) == candidate.get("schema_version", 1) and baseline.get("metric_units") != candidate.get("metric_units"):
        reasons.append("statistical units differ")
    if baseline.get("scenario", {}).get("assertions", []) != candidate.get("scenario", {}).get("assertions", []):
        reasons.append("acceptance criteria differ")
    def effective_args(result):
        args, normalized, skip = result.get("runtime_arguments", []), [], False
        for arg in args:
            if skip:
                skip = False
            elif arg == "--file-prefix":
                skip = True
            elif not arg.startswith("--file-prefix="):
                normalized.append(arg)
        return normalized
    if effective_args(baseline) != effective_args(candidate):
        reasons.append("runtime arguments differ")
    for key in ("cpu_model", "machine", "kernel", "numa", "link_mbps", "worker_lcores"):
        if baseline["environment"].get(key) != candidate["environment"].get(key):
            reasons.append("environment differs: " + key)
    for key in ("driver", "device", "rx_mode", "rxq", "txq", "workers", "main", "direct_rx", "direct_tx"):
        if baseline["environment"].get("dataplane", {}).get(key) != candidate["environment"].get("dataplane", {}).get(key):
            reasons.append("dataplane differs: " + key)
    metrics = {key: change(baseline.get("summary", {}).get(key), candidate.get("summary", {}).get(key))
               for key in ("success_rps", "transaction_success_tps", "request_success_rps", "p95_ms", "p99_ms", "error_rate", "maximum_sustainable_cps")}
    metrics["error_rate"]["delta_percentage_points"] = (
        metrics["error_rate"]["delta"] * 100 if metrics["error_rate"]["delta"] is not None else None)
    if any(metrics[k]["baseline"] is None or metrics[k]["candidate"] is None
           for k in ("success_rps", "p95_ms", "p99_ms", "error_rate")):
        reasons.append("missing throughput/latency/error-rate samples")
    regressions = []
    for key, direction in (("success_rps", -1), ("p95_ms", 1), ("p99_ms", 1)):
        delta = metrics[key]["percent"]
        if (delta is not None and delta * direction > tolerance) or (
                metrics[key]["baseline"] == 0 and (metrics[key]["candidate"] or 0) > 0 and direction == 1):
            regressions.append(key)
    if metrics["error_rate"]["delta"] is not None and metrics["error_rate"]["delta"] > 0:
        regressions.append("error_rate")
    new_errors = sorted(k for k, v in candidate.get("errors", {}).items() if v and not baseline.get("errors", {}).get(k))
    resources = {k: change(baseline.get("resource_peaks", {}).get(k, {}).get("value"), v["value"])
                 for k, v in candidate.get("resource_peaks", {}).items()}
    has_slo = any(a.get("critical", True) for a in candidate.get("assertions", []))
    blocked = candidate.get("status") != "passed" or bool(regressions or new_errors)
    recommendation = "inconclusive" if reasons or not has_slo else "reject" if blocked else "accept_within_tested_scope"
    return dict(comparable=not reasons, reasons=reasons, tolerance_percent=tolerance, metrics=metrics,
                resources=resources, new_error_types=new_errors, regressions=regressions,
                recommendation=recommendation, limitations=["Single-run comparison is not a statistical significance test.",
                "Maximum sustainable load is not determined by these runs."] + ([] if has_slo else ["No critical SLO assertions supplied."]))


def report(result):
    """Render escaped, self-contained HTML/SVG; timeline lines remain per worker."""
    esc = lambda value: html.escape(str(value))
    def table(headers, rows):
        return "<table><thead><tr>" + "".join("<th>" + esc(h) + "</th>" for h in headers) + "</tr></thead><tbody>" + "".join(
            "<tr>" + "".join("<td>" + esc(c) + "</td>" for c in row) + "</tr>" for row in rows) + "</tbody></table>"
    def pre(value):
        return "<pre>" + esc(json.dumps(value, ensure_ascii=False, indent=2)) + "</pre>"
    env, build, scenario = result.get("environment", {}), result.get("build", {}), result.get("scenario", {})
    meta = env.get("dataplane", {})
    memory = re.search(r"^MemTotal:\s*(.*)$", env.get("memory") or "", re.M)
    comparison = result.get("comparison")
    if comparison:
        baseline_html = "<p>建议：" + esc(comparison["recommendation"]) + "</p>" + table(
            ["指标", "基线", "本次", "差值", "变化 %", "说明"],
            [(k, v["baseline"], v["candidate"], v["delta"], v["percent"], v["reason"])
             for k, v in comparison["metrics"].items()]) + "<p>错误率差值为比例，乘 100 得百分点。</p>" + pre(
                {k: v for k, v in comparison.items() if k != "metrics"})
    else:
        baseline_html = "<p>未提供基线。</p>"
    parts = ["<!doctype html><html lang='zh'><meta charset='utf-8'><title>SnowTG report</title>",
             "<style>body{font:15px system-ui;max-width:1100px;margin:40px auto;padding:0 20px;color:#172532}table{border-collapse:collapse;width:100%;margin:16px 0}td,th{border:1px solid #ccd5df;padding:8px;text-align:left}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#f1f4f7;padding:16px}svg{width:100%;height:210px;background:#f5f7fa}h2{margin-top:32px}</style>",
             "<h1>SnowTG 测试报告</h1><p>" + esc(result.get("purpose", "")) + "</p>",
             "<p>状态：" + esc(result.get("status", "invalid")) + "</p>",
             "<h2>环境与版本</h2>", table(["项目", "值"], [
                ("CPU / 核数", f"{env.get('cpu_model')} / {env.get('logical_cpus')}"),
                ("内存", memory.group(1) if memory else "unknown"),
                ("NUMA", canonical(env.get("numa", {}))),
                ("网卡 / 驱动 / 链路 Mbps", f"{meta.get('device')} / {meta.get('driver')} / {env.get('link_mbps')}"),
                ("Main / worker lcores", f"{meta.get('main')} / {env.get('worker_lcores')}"),
                ("Git commit（构建时）", build.get("git_commit")),
                ("原生二进制 SHA256", build.get("binary_sha256")),
                ("DPDK", build.get("dpdk_version")), ("被测服务（用户声明）", canonical(result.get("service", {})))]),
             "<details><summary>完整环境、构建参数和启动命令</summary>", pre(dict(environment=result.get("environment"), service=result.get("service"),
                service_metadata_source=result.get("service_metadata_source"),
                command=result.get("command"), build={k: v for k, v in build.items() if k != "sources"})), "</details>",
             "<h2>流量模型</h2>", table(["阶段", "持续秒", "起始 CPS", "目标 CPS"],
                [(p["name"], p["duration_sec"], p.get("start_cps", p["target_cps"]), p["target_cps"])
                 for p in phases(scenario)]), "<details><summary>完整剧本</summary>", pre(scenario), "</details>",
             "<h2>关键结果</h2>", table(["指标", "值"], result.get("summary", {}).items()),
             "<h2>SLO 验收</h2>", table(["指标 / 筛选条件", "实际值", "要求", "通过", "原因"],
                [(canonical({k: v for k, v in a.items() if k not in ("actual", "passed", "reason", "value", "op")}),
                  a["actual"], f"{a['op']} {a['value']}", a["passed"], a["reason"]) for a in result.get("assertions", [])]),
             "<h2>阶段与流量类别</h2>", table(["阶段", "类别", "协议", "计划", "成功", "跳过", "启动失败", "成功完成 P99 μs"],
                [(g["phase"], g["class"], g["protocol"], g["planned"], g["success"], g["skipped"], g["start_failed"],
                  g.get("latency", {}).get("complete_success", {}).get("p99_us")) for g in result.get("groups", [])]),
             "<h2>基线差异</h2>", baseline_html,
             "<h2>随时间变化（每条线一个 worker）</h2>"]
    if result.get("steps"):
        parts.extend(["<h2>业务步骤（事务 TPS 与网络请求 RPS 分列于关键结果）</h2>", table(
            ["阶段", "类别", "步骤", "类型", "到达", "成功", "失败含启动失败", "分支未执行", "上游失败未到达", "成功 P99 μs"],
            [(g["phase"],g["class"],g["step"],g["protocol"],g["reached"],g["success"],g["failed"]+g["start_failed"],
              g["branch_skipped"],g["not_reached"],g["latency"]["complete_success"]["p99_us"]) for g in result["steps"]]),
            "<p>思考耗时单列；事务延迟包含思考及后续步骤等待。步骤成功率以到达数为分母。</p>",
            "<h2>事务上下文预分配内存（响应捕获额外按需分配）</h2>",pre(result.get("workflow_memory",[]))])
    timeline = result.get("timeline", [])
    for key, title in (("success_rps", "成功 RPS"), ("complete_mean_us", "区间平均终止延迟 μs"),
                       ("completed_error_rate", "已完成事务错误率"), ("active", "在途事务数"),
                       ("live_sockets", "存活 socket"), ("tx_peak", "TX 对象历史峰值"), ("payload_peak", "Payload 对象历史峰值")):
        points = [r for r in timeline if r.get(key) is not None]
        parts.append("<h3>" + title + "</h3>")
        if not points:
            parts.append("<p>无样本</p>")
            continue
        xmax = max(max(r["time_sec"] for r in points), 1)
        ymax = max(max(r[key] for r in points), 1)
        parts.append(f"<p>横轴 0～{xmax:.2f} 秒；纵轴 0～{ymax:.3g}</p><svg viewBox='0 0 1000 210' role='img' aria-label='{title}'>")
        for worker in sorted({r["worker"] for r in points}):
            coords = " ".join(f"{20+960*r['time_sec']/xmax:.2f},{190-170*r[key]/ymax:.2f}" for r in points if r["worker"] == worker)
            color = ("#1768ac", "#b43e53", "#18855c", "#9854ac")[worker % 4]
            parts.append(f"<polyline fill='none' stroke='{color}' stroke-width='2' points='{coords}'><title>worker {worker}</title></polyline>")
        parts.append("</svg>")
    parts.extend(["<h2>有效性与限制</h2>", pre(result.get("invalid_reasons", [])), pre(result.get("limitations", LIMITS)),
                  "<h2>结论</h2><p>" + esc(result.get("comparison", {}).get("recommendation", result.get("status", "invalid"))) +
                  "；结论只覆盖本次负载、持续时间与环境。</p></html>"])
    return "\n".join(parts)


def cli(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    comp = sub.add_parser("compare")
    comp.add_argument("baseline", type=Path)
    comp.add_argument("candidate", type=Path)
    comp.add_argument("--tolerance-percent", type=float, default=5)
    comp.add_argument("--output", type=Path)
    rep = sub.add_parser("report")
    rep.add_argument("result", type=Path)
    rep.add_argument("--baseline", type=Path)
    rep.add_argument("--output", type=Path, required=True)
    opts = parser.parse_args(argv)
    if opts.command == "compare":
        if not math.isfinite(opts.tolerance_percent) or opts.tolerance_percent < 0:
            raise ValueError("tolerance must be finite and nonnegative")
        data = compare(load_result(opts.baseline), load_result(opts.candidate), opts.tolerance_percent)
        print(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False))
        if opts.output:
            if opts.output.resolve() in (opts.baseline.resolve(), opts.candidate.resolve()):
                raise ValueError("comparison must not overwrite input result JSON")
            write_json(opts.output, data)
        return 0 if data["recommendation"] == "accept_within_tested_scope" else 2
    data = load_result(opts.result)
    if opts.baseline:
        data["comparison"] = compare(load_result(opts.baseline), data)
    if opts.output.resolve() in [p.resolve() for p in (opts.result, opts.baseline) if p]:
        raise ValueError("report must not overwrite result JSON")
    opts.output.write_text(report(data), encoding="utf-8")
    return 0
