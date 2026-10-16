#!/usr/bin/env python3
"""Replay SHA-pinned original-instruction outputs against production functions.

Expected values are exported by the parent project's compare-steam-video-recovery.py
from the user-supplied original ELF. This runner requires neither that ELF nor Unicorn.
It never updates expected values from the current implementation's output.
"""
import argparse
import json
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--media", required=True)
parser.add_argument("--video", required=True)
parser.add_argument("--fixture", required=True)
args = parser.parse_args()
with open(args.fixture) as stream:
    fixture = json.load(stream)
assert (
    fixture["binary_sha256"]
    == "50e1d3147d5d47b71ef1970e1867a2fe4f3e1ecf2ea6153f927fac88b88ea38e"
)
failures = []
for case in fixture["cases"]:
    command = (
        [args.media, str(case["rtt"]), str(case["smooth"])]
        if case["driver"] == "media"
        else [args.video, "--oracle"]
    )
    run = subprocess.run(
        command,
        input="\n".join(case["input"]) + "\n",
        text=True,
        capture_output=True,
        timeout=5,
    )
    actual = [
        json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")
    ]
    if run.returncode or actual != case["expected"]:
        failures.append(case["case"])
        print(
            json.dumps(
                {
                    "case": case["case"],
                    "returncode": run.returncode,
                    "expected": case["expected"],
                    "actual": actual,
                    "stderr": run.stderr,
                }
            )
        )
print(json.dumps({"cases": len(fixture["cases"]), "failed": failures}))
raise SystemExit(bool(failures))
