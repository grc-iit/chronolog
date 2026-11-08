#!/usr/bin/env python3
"""Exercise retained managed-stop attempts inside the existing test guard."""
import json
from pathlib import Path
import subprocess
import sys
import time


def managed_stop(work, deploy, log, guard_seconds=240, runner=subprocess.run,
                 now=time.monotonic, pause=time.sleep):
    deadline = now() + guard_seconds
    attempt = 0
    with Path(log).open("w") as evidence:
        while True:
            remaining = deadline - now()
            # Each controller attempt keeps its shipped 30000 ms budget. A
            # 35-second command guard plus two-second signal grace must fit.
            if remaining < 37:
                print("Managed stop did not complete inside the original test guard", file=evidence)
                return 1
            attempt += 1
            command = ["timeout", "--signal=TERM", "--kill-after=2s", "35s",
                       str(deploy), "-s", "-w", str(work)]
            result = runner(command, capture_output=True, text=True)
            output = (result.stdout or "") + (result.stderr or "")
            evidence.write(f"managed stop attempt {attempt} command status {result.returncode}\n")
            evidence.write(output)
            evidence.flush()
            print(output, end="", flush=True)
            records = []
            for line in output.splitlines():
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if isinstance(record, dict) and record.get("operation") == "stop" and record.get("aggregate") is True:
                    records.append(record)
            status = records[0].get("status") if len(records) == 1 else None
            if now() > deadline:
                return 1
            if result.returncode == 0 and status == 0:
                return 0
            # Only an actual completed -27 controller attempt may be retried.
            # Timeout, missing evidence or a contradictory exit code fails.
            if result.returncode != 27 or status != -27:
                return 1
            pause(min(3, max(0, deadline - now())))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: managed_stop_check.py WORK_DIR DEPLOY_SCRIPT LOG")
    raise SystemExit(managed_stop(*sys.argv[1:]))
