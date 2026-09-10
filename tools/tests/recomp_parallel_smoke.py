#!/usr/bin/env python3
"""Compare serial and repeated parallel single-file codegen using a local TOML config.

Example: python3 tools/tests/recomp_parallel_smoke.py build/ps2xRecomp/ps2_recomp
         games/bt3/work/overlay/config_dbzp_gaps.toml
No game data or generated code is included in this test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recompiler", type=Path)
    parser.add_argument("config", type=Path)
    parser.add_argument("--rounds", type=int, default=20)
    args = parser.parse_args()
    config = args.config.read_text()
    # Drop existing settings before injecting our test settings into [general].
    config = re.sub(r"(?m)^\s*(output|single_file_output|output_worker_threads|low_memory_mode)\s*=.*$", "", config)
    reference = None
    with tempfile.TemporaryDirectory(prefix="bt3-codegen-test-") as tmp:
        root = Path(tmp)
        for iteration in range(args.rounds + 1):
            workers = 1 if iteration == 0 else (2 if iteration % 2 else 10)
            settings = (f"[general]\noutput = {json.dumps(str(root / 'output') + '/')}\n"
                        f"single_file_output = true\noutput_worker_threads = {workers}\nlow_memory_mode = false")
            cfg = root / "test.toml"
            cfg.write_text(config.replace("[general]", settings, 1))
            result = subprocess.run([str(args.recompiler.resolve()), str(cfg)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            digest = hashlib.sha256((root / "output/ps2_recompiled_functions.cpp").read_bytes()).hexdigest()
            if reference is None:
                reference = digest
            assert digest == reference, f"Codegen differs with {workers} workers on iteration {iteration}"
    print(f"PASS: {args.rounds} parallel runs match serial output ({reference})")


if __name__ == "__main__":
    main()
