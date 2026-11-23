#!/usr/bin/env python3
"""Assemble an immutable release; does not install or start services.

The bundle contains only the binaries and Python modules the worker uses. The
old per-programme profile files are intentionally not carried over: detection
is learned from each recording.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

# Every worker module is copied, so a module added later cannot be forgotten.
# A fixed list once left cm_scenario.py out, and the logo-less path failed on
# every recording with "No module named".
PROGRAMMES = sorted(path.name for path in Path(__file__).resolve().parent.glob('cm_*.py'))
BINARIES = ['cm-cut', 'cm-verify', 'cm-scan', 'cm-overlay']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary_root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = Path(__file__).parent
    output = args.output
    output.mkdir(parents=True, exist_ok=False)
    for name in PROGRAMMES:
        shutil.copy2(source / name, output / name)
    for name in BINARIES:
        shutil.copy2(args.binary_root / name, output / name)
    manifest = {str(path.relative_to(output)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(output.rglob('*')) if path.is_file()}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    print(json.dumps(dict(output=str(output), files=len(manifest))))


if __name__ == '__main__':
    main()
