"""Record build-time provenance after linking, tied to the executable SHA256.

Sources cover native C/H files, not launcher scripts (hashed by managed runs).
Missing tools/Git metadata remain null; mirrored trees may set SNOWTG_SOURCE_COMMIT.
Consumers must verify binary_sha256 before trusting an existing sidecar.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def command(*args):
    try:
        result = subprocess.run(args, text=True, capture_output=True)
    except OSError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    for flag in ("cc", "cflags", "ldflags"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = {}
    for directory in ("traffic-gen", "pro-stack", "third_party"):
        for path in sorted((root / directory).rglob("*")):
            if path.is_file() and path.suffix in (".c", ".h") and not any(
                    part.startswith("build") for part in path.relative_to(root).parts):
                sources[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    data = dict(binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                git_commit=command("git", "rev-parse", "HEAD") or os.environ.get("SNOWTG_SOURCE_COMMIT"),
                git_status=command("git", "status", "--porcelain"),
                compiler=args.cc, compiler_version=command(args.cc, "--version"),
                cflags=args.cflags, ldflags=args.ldflags,
                dpdk_version=command("pkg-config", "--modversion", "libdpdk"),
                source_sha256=hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest(),
                sources=sources)
    args.binary.with_suffix(".build.json").write_text(json.dumps(data, indent=2) + "\n")
