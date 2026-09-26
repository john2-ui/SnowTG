"""Startup-only dataset loading. Native workers receive immutable scalar rows."""
import csv
import hashlib
import io
import json
import math
from pathlib import Path

# Aggregate budget across classes: raw file bytes or encoded inline-row bytes.
LIMIT = 16 * 1024 * 1024


def unique_fields(pairs):
    """Reject ambiguous JSON rows rather than silently keeping the last key."""
    row = {}
    for key, value in pairs:
        if key in row:
            raise ValueError('duplicate dataset field: ' + key)
        row[key] = value
    return row


def expand_datasets(plan, directory):
    """Resolve files relative to the scenario, enforce the total input budget.

    CSV values remain strings; JSON retains scalar types. File hashes document
    the source while workload hashing includes the expanded rows themselves.
    """
    changed, total, sources = False, 0, []
    for cls in plan.get('classes', []):
        transaction = cls.get('transaction', {})
        rows = transaction.get('dataset')
        is_csv = False
        if rows is None:
            continue
        if isinstance(rows, dict):
            if set(rows) != {'file'}:
                raise ValueError('dataset file reference must contain only file')
            # Resolve from the scenario directory, not the launcher's working directory.
            # Check the remaining budget before reading file contents.
            path = (Path(directory) / rows['file']).resolve()
            if path.stat().st_size > LIMIT - total:
                raise ValueError('datasets exceed 16 MiB')
            raw = path.read_bytes()
            total += len(raw)
            text = raw.decode('utf-8-sig')
            if path.suffix.lower() == '.csv':
                is_csv = True
                # Let the standard library handle quoting, commas, and embedded newlines.
                # CSV values remain strings; JSON datasets retain scalar types.
                reader = csv.DictReader(io.StringIO(text, newline=''))
                names = reader.fieldnames
                if not names or len(names) != len(set(names)) or any(not n for n in names):
                    raise ValueError('CSV requires unique nonempty column names')
                rows = list(reader)
            elif path.suffix.lower() == '.json':
                rows = json.loads(text, object_pairs_hook=unique_fields)
            else:
                raise ValueError('dataset file must be CSV or JSON')
            # Keep the original content digest for provenance, then send expanded rows
            # to native workers so runtime execution never depends on this path.
            sources.append(dict(file=str(path), sha256=hashlib.sha256(raw).hexdigest(), bytes=len(raw)))
            changed = True
        else:
            total += len(json.dumps(rows, ensure_ascii=False, allow_nan=False).encode())
        if total > LIMIT or not isinstance(rows, list) or not rows:
            raise ValueError('dataset requires nonempty rows within 16 MiB')
        # Validate row/field/value bounds before handing immutable data to C.
        for row in rows:
            if not isinstance(row, dict) or len(row) > 32:
                raise ValueError('dataset rows must be objects with at most 32 fields')
            for key, value in row.items():
                if not isinstance(key, str) or not key or '\x00' in key or len(key.encode()) > 63:
                    raise ValueError('invalid dataset field name')
                if type(value) not in (str, int, float, bool, type(None)) or isinstance(value, float) and not math.isfinite(value):
                    raise ValueError('dataset values must be finite JSON scalars')
                if isinstance(value, str) and ('\x00' in value or len(value.encode()) > 1024):
                    raise ValueError('dataset string exceeds 1 KiB or contains NUL')
            # DictReader uses a None key for extra columns and None values for missing
            # columns. Both are malformed rows, not empty-string or JSON-null values.
            if None in row or is_csv and any(v is None for v in row.values()):
                raise ValueError('CSV row width differs from header')
        transaction['dataset'] = rows
    if sources:
        plan['dataset_sources'] = sources
    return changed
