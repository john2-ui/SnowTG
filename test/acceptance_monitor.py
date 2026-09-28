#!/usr/bin/env python3
"""Compatibility entry point; prefer `snowtg.py monitor` for host sampling."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'traffic-gen'))
from snowtg_monitor import main, sample

if __name__ == '__main__':
    sys.exit(main())
