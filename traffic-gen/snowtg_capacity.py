"""Repeated fixed-rate SLO search and conservative capacity regression gates."""
from copy import deepcopy
from datetime import datetime, timezone
from decimal import Decimal
import html
import json
import math
from pathlib import Path
import uuid

import snowtg_results as results

LIMITS = [
    "Finite-duration repeatability, not proof of indefinite sustainability.",
    "Search assumes monotone SLO behavior; noise can move the bracket.",
    "A passing maximum establishes a lower bound, not an exact capacity.",
    "CPS is offered transaction starts/second, not successful RPS or TCP connections/second.",
]


def validate_options(minimum, maximum, precision, repeats, max_drop_percent=5, timeout=None):
    if any(type(v) is not int or v <= 0 for v in (minimum, maximum, precision, repeats)) or minimum > maximum:
        raise ValueError("require 0 < minimum <= maximum, positive integer precision and repeats")
    if not math.isfinite(max_drop_percent) or not 0 <= max_drop_percent < 100:
        raise ValueError("capacity drop percent must be finite and in [0,100)")
    if timeout is not None and (not math.isfinite(timeout) or timeout <= 0):
        raise ValueError("timeout must be positive and finite")


def search(probe, minimum, maximum, precision):
    """Find a pass/fail bracket, assuming monotone SLO behavior within this run."""
    validate_options(minimum, maximum, precision, 1)
    low, high = 0, minimum
    if not probe(high):
        return dict(status="below_minimum", passed_cps=None, failed_cps=high)
    low = high
    while low < maximum:
        high = min(maximum, low * 2)
        if not probe(high):
            break
        low = high
    if low == maximum:
        return dict(status="lower_bound_only", passed_cps=low, failed_cps=None)
    while high - low > precision:
        mid = (low + high) // 2
        if probe(mid):
            low = mid
        else:
            high = mid
    return dict(status="bracketed", passed_cps=low, failed_cps=high)


def workload_hash(plan):
    # Each probe changes only target_cps; metadata and declared service versions
    # are not the offered workload. Dataset content remains in the expanded plan.
    return results.digest({k: v for k, v in plan.items() if k not in results.EXTRA | {"target_cps"}})


def validate_result(result):
    """Reject malformed capacity bounds before using an artifact as a baseline."""
    if result.get("kind") != "capacity":
        raise ValueError("expected a capacity result")
    if any(not isinstance(result.get(k), dict) for k in ("capacity", "search", "summary", "scenario", "environment")):
        raise ValueError("incomplete capacity result")
    capacity = result.get("capacity", {})
    status, low, high = (capacity.get(k) for k in ("status", "passed_cps", "failed_cps"))
    positive = lambda v: type(v) is int and v > 0
    valid_bounds = (
        status == "bracketed" and positive(low) and positive(high) and low < high or
        status == "lower_bound_only" and positive(low) and high is None or
        status == "below_minimum" and low is None and positive(high))
    if result.get("valid"):
        if not valid_bounds or result.get("status") != status:
            raise ValueError("inconsistent capacity bounds/status")
        config = result["search"]
        validate_options(config["minimum"], config["maximum"], config["precision"], config["repeats"])
        if (low is not None and not config["minimum"] <= low <= config["maximum"] or
            high is not None and not config["minimum"] <= high <= config["maximum"] or
            status == "bracketed" and high - low > config["precision"] or
            status == "lower_bound_only" and low != config["maximum"] or
            status == "below_minimum" and high != config["minimum"]):
            raise ValueError("capacity bounds disagree with search configuration")
        expected = low if status == "bracketed" else None
        if result["summary"].get("maximum_sustainable_cps") != expected:
            raise ValueError("inconsistent maximum_sustainable_cps estimate")
        if result["summary"].get("sustainable_cps_lower_bound") != low:
            raise ValueError("inconsistent sustainable_cps_lower_bound")
        if "phases" in result["scenario"] or "duration_sec" not in result["scenario"] or "target_cps" not in result["scenario"]:
            raise ValueError("capacity result requires a fixed-duration, fixed-rate scenario")
        results.validate_assertions(result["scenario"])
        if not any(a.get("critical", True) for a in result["scenario"].get("assertions", [])):
            raise ValueError("capacity result requires a critical SLO")
        if result.get("workload_sha256") != workload_hash(result["scenario"]):
            raise ValueError("capacity workload digest mismatch")


def compare_capacity(baseline, candidate, tolerance=5):
    """Gate intervals, never equate two measured lower bounds with equal capacity.

    Accept if candidate's passing rate covers even baseline's failing bound after
    the allowed drop. Reject if candidate fails at/below baseline's passing rate
    after that drop. Overlapping/unknown bounds cannot decide the gate.
    """
    validate_options(1, 1, 1, 1, tolerance)
    reasons = []
    if baseline.get("kind") != "capacity" or candidate.get("kind") != "capacity":
        reasons.append("cannot compare capacity and single-run results")
    else:
        validate_result(baseline)
        validate_result(candidate)
        reasons = results.comparison_reasons(baseline, candidate)
        if baseline.get("search", {}).get("repeats") != candidate.get("search", {}).get("repeats"):
            reasons.append("repeat counts differ")
        if baseline.get("search", {}).get("timeout_sec") != candidate.get("search", {}).get("timeout_sec"):
            reasons.append("measurement timeouts differ")
    comparable = not reasons
    old, new = baseline.get("capacity", {}), candidate.get("capacity", {})
    old_low, old_high = old.get("passed_cps"), old.get("failed_cps")
    new_low, new_high = new.get("passed_cps"), new.get("failed_cps")
    recommendation, code = "inconclusive", 1
    factor = 1 - Decimal(str(tolerance)) / 100
    if comparable:
        if old_low is None:
            reasons.append("baseline has no passing rate")
        elif old_high is not None and new_low is not None and Decimal(new_low) >= Decimal(old_high) * factor:
            recommendation, code = "accept_within_tested_scope", 0
        elif new_high is not None and Decimal(new_high) <= Decimal(old_low) * factor:
            recommendation, code = "reject", 2
        else:
            reasons.append("capacity intervals cannot decide the gate; raise the ceiling or reduce precision")
    return dict(comparable=comparable, reasons=reasons, tolerance_percent=tolerance,
                baseline_bounds=old, candidate_bounds=new,
                metrics={"maximum_sustainable_cps": results.change(
                    baseline.get("summary", {}).get("maximum_sustainable_cps"),
                    candidate.get("summary", {}).get("maximum_sustainable_cps"))},
                recommendation=recommendation, exit_code=code, limitations=LIMITS[:])


def run_capacity(plan, binary, args, output=None, *, minimum=100, maximum=10000,
                 precision=100, repeats=2, timeout=None, baseline=None, max_drop_percent=5):
    validate_options(minimum, maximum, precision, repeats, max_drop_percent, timeout)
    if "phases" in plan or "duration_sec" not in plan or "target_cps" not in plan:
        raise ValueError("capacity requires a fixed-duration, fixed-rate scenario")
    results.validate_assertions(plan)
    if not any(a.get("critical", True) for a in plan.get("assertions", [])):
        raise ValueError("capacity requires at least one critical SLO")
    baseline_data = results.load_result(baseline) if baseline else None
    if baseline_data is not None and baseline_data.get("kind") != "capacity":
        raise ValueError("capacity baseline must be a capacity result")
    if baseline_data is not None and not baseline_data.get("valid"):
        raise ValueError("capacity baseline is invalid")
    output = Path(output or ("debug/capacity-" + uuid.uuid4().hex[:8])).resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = dict(schema_version=results.SCHEMA, kind="capacity", status="running", valid=False,
        started_utc=datetime.now(timezone.utc).isoformat(), scenario=deepcopy(plan),
        scenario_sha256=results.digest(plan), workload_sha256=workload_hash(plan),
        purpose=plan.get("purpose", plan.get("name")), service=plan.get("service", {}),
        search=dict(minimum=minimum, maximum=maximum, precision=precision, repeats=repeats, timeout_sec=timeout),
        capacity=dict(status="running", passed_cps=None, failed_cps=None),
        summary=dict(maximum_sustainable_cps=None, sustainable_cps_lower_bound=None),
        environment={}, build={}, runtime_arguments=[], rounds=[], invalid_reasons=[],
        exit_code=1, limitations=LIMITS[:])
    reference = None

    def save():
        results.write_json(output / "result.json", result)

    def probe(cps):
        nonlocal reference
        passed = True
        for repeat in range(1, repeats + 1):
            trial = deepcopy(plan)
            trial["target_cps"] = cps
            directory = output / f"cps-{cps}-r{repeat}"
            code = results.run(trial, binary, args, output=directory, timeout=timeout)
            measured = results.load_result(directory / "result.json")
            result["rounds"].append(dict(cps=cps, repeat=repeat, result=f"{directory.name}/result.json",
                exit_code=code, status=measured["status"], summary=measured["summary"]))
            save()
            if code not in (0, 2) or not measured.get("valid") or measured.get("exit_code") != code or measured["status"] != ("passed" if code == 0 else "failed"):
                raise ValueError(f"invalid measurement at {directory}")
            normalized = dict(measured, workload_sha256=workload_hash(measured["scenario"]))
            if reference is None:
                reference = normalized
                for key in ("environment", "build", "runtime_arguments", "metric_units", "controller_sha256"):
                    if key in measured:
                        result[key] = measured[key]
            else:
                reasons = results.comparison_reasons(reference, normalized)
                for key in ("build", "controller_sha256"):
                    if reference.get(key) != normalized.get(key):
                        reasons.append(key + " changed during search")
                if reasons:
                    raise ValueError("inconsistent measurement: " + "; ".join(reasons))
            passed = passed and code == 0
        return passed

    save()
    try:
        bounds = search(probe, minimum, maximum, precision)
        result.update(status=bounds["status"], capacity=bounds, valid=True,
                      exit_code=2 if bounds["status"] == "below_minimum" else 0)
        result["summary"].update(sustainable_cps_lower_bound=bounds["passed_cps"],
            maximum_sustainable_cps=bounds["passed_cps"] if bounds["status"] == "bracketed" else None)
        if baseline_data is not None:
            result["comparison"] = compare_capacity(baseline_data, result, max_drop_percent)
            result["exit_code"] = result["comparison"]["exit_code"]
    except (OSError, ValueError, KeyError, TypeError, KeyboardInterrupt) as error:
        result.update(status="invalid", valid=False, exit_code=1,
                      capacity=dict(status="invalid", passed_cps=None, failed_cps=None))
        result["summary"].update(maximum_sustainable_cps=None, sustainable_cps_lower_bound=None)
        result["invalid_reasons"].append(str(error) or "interrupted")
    save()
    (output / "report.html").write_text(report_capacity(result), encoding="utf-8")
    print(f"{result['status']}: {output / 'result.json'} (exit {result['exit_code']})")
    return result["exit_code"]


def report_capacity(result):
    esc = lambda value: html.escape(str(value))
    sections = ["<!doctype html><html lang='en'><meta charset='utf-8'><title>SnowTG capacity</title>",
                "<h1>SnowTG capacity</h1><p>" + esc(result.get("purpose", "")) + "</p>",
                "<p>maximum_sustainable_cps is the passing endpoint estimate of a bracket, not an exact maximum.</p>"]
    for key in ("status", "exit_code", "summary", "capacity", "search", "comparison", "scenario",
                "environment", "build", "runtime_arguments", "invalid_reasons", "limitations"):
        sections.append("<h2>" + key + "</h2><pre>" + esc(json.dumps(result.get(key), ensure_ascii=False, indent=2)) + "</pre>")
    sections.append("<h2>Measurements</h2><ul>")
    for row in result.get("rounds", []):
        sections.append("<li>" + esc(f"{row['cps']} CPS / repeat {row['repeat']}: {row['status']} — {row['result']}") + "</li>")
    return "\n".join(sections + ["</ul></html>"])
