"""Audit active component fields in the actual, identity-bound launch records."""
import collections
import json
from pathlib import Path
import sys


def values(node, key):
    found = []
    if isinstance(node, dict):
        for name, value in node.items():
            if name == key:
                found.append(value)
            else:
                found.extend(values(value, key))
    elif isinstance(node, list):
        for value in node:
            found.extend(values(value, key))
    return found


def audit(work, protocol, expected):
    if protocol not in ("ofi+sockets", "ofi+tcp"):
        raise ValueError("unsupported protocol")
    work = Path(work)
    manifest = json.loads((work / "monitor/lifecycle/deployment.json").read_text())
    records = manifest.get("records", [])
    counts = collections.Counter(record["component"] for record in records)
    if not records or dict(counts) != expected:
        raise ValueError(f"wrong actual component inventory: {dict(counts)}")
    for record in records:
        component = record["component"]
        config = Path(record["config"])
        executable = record["executable"]
        if record["argv"] != [executable, "--config", str(config)] or Path(executable).name != "chrono-" + component:
            raise ValueError("launch argv/executable binding mismatch")
        if config.read_text() != record["config_bytes"]:
            raise ValueError("config changed after launch")
        status = json.loads(Path(record["status_file"]).read_text())
        for key in ("pid", "uid", "executable", "config", "host", "start_ticks", "boot_id"):
            if status.get(key) != record.get(key):
                raise ValueError(f"native launch identity mismatch: {key}")
        if status.get("deployment_id") != manifest["id"] or status.get("native_proof", {}).get("generation", 0) <= 0:
            raise ValueError("native deployment/generation binding mismatch")
        active = json.loads(record["config_bytes"])["chrono_" + component]
        rpc = values(active, "protocol_conf")
        extraction = values(active, "extraction_protocol")
        expected_extraction = 1 if component in ("keeper", "grapher") else 0
        if not rpc or any(value != protocol for value in rpc):
            raise ValueError(f"wrong active {component} RPC protocol")
        if len(extraction) != expected_extraction or any(value != protocol for value in extraction):
            raise ValueError(f"wrong active {component} extraction protocol")
        print(f"audited: {component} {record['host']} pid={record['pid']} {config.name} RPC={rpc} extraction={extraction}")
    return len(records)


if __name__ == "__main__":
    if len(sys.argv) != 6:
        raise SystemExit("usage: rpc_config_audit.py WORK PROTOCOL KEEPERS GRAPHERS PLAYERS")
    expected = dict(visor=1, keeper=int(sys.argv[3]), grapher=int(sys.argv[4]), player=int(sys.argv[5]))
    print("Audited launched component configs:", audit(sys.argv[1], sys.argv[2], expected))
