"""Check retained shutdown data through public appends and the production reader."""
import datetime
import calendar
import json
from pathlib import Path
import subprocess
import socket
import sys
import time

from managed_stop_check import managed_stop


def unsealed_until(events, release_started, chunk_seconds, acceptance_seconds):
    tick = chunk_seconds * 1_000_000_000
    grace = acceptance_seconds * 1_000_000_000
    return min([release_started + grace] + [(event[0] // tick + 1) * tick + grace for event in events])


def shutdown_before_seal(text, chronicle, boundary, stop_started=0, stop_finished=2**64):
    holder_line = next((line for line in text.splitlines()
                        if f"Chronicle={chronicle}, Story=retained," in line), None)
    if holder_line is None:
        return False
    marker = "[KeeperDataStore] Initiating shutdown of DataCollection."
    for line in text.splitlines():
        if marker in line:
            stamp = datetime.datetime.strptime(line[1:24], "%Y-%m-%d %H:%M:%S.%f").replace(tzinfo=datetime.timezone.utc)
            stamp_ns = calendar.timegm(stamp.utctimetuple()) * 1_000_000_000 + stamp.microsecond * 1000
            if stamp_ns + 1_000_000 < stop_started or stamp_ns > stop_finished:
                continue
            # Standard log timestamps have millisecond resolution. Use the end
            # of that interval; file arrival/flush time is not event time.
            if stamp_ns + 1_000_000 <= boundary:
                return dict(shutdown_timestamp_ns=stamp_ns,
                            shutdown_timestamp_upper_ns=stamp_ns + 1_000_000,
                            shutdown_line=line, holder_line=holder_line)
            raise ValueError("the retained test story could seal before Keeper shutdown began")
    raise ValueError("missing actual Keeper DataCollection shutdown timestamp")


def keeper_shutdown_observation(record, logs, chronicle, boundary, stop_started,
                                stop_finished, generation, deployment_id):
    text = "\n".join(content for _, content in logs)
    if "shutdown with unconfirmed" in text:
        raise ValueError("unconfirmed data at Keeper exit")
    selected = shutdown_before_seal(text, chronicle, boundary, stop_started, stop_finished)
    if not selected:
        return None
    for kind in ("holder", "shutdown"):
        source_line = selected.pop(kind + "_line")
        selected[kind + "_source"] = next(
            dict(file=str(path), line_number=index, line=line)
            for path, content in logs
            for index, line in enumerate(content.splitlines(), 1) if line == source_line)
    selected["identity"] = {key: record[key] for key in
                            ("component", "host", "pid", "uid", "executable", "config", "start_ticks", "boot_id")}
    selected["identity"].update(generation=generation, deployment_id=deployment_id)
    return selected


def persist_summary(summary, directory=Path("/tmp")):
    directory = Path(directory)
    immutable = directory / ("shutdown_data_check." + summary["chronicle"] + ".json")
    payload = json.dumps(dict(summary, evidence_path=str(immutable)), indent=2) + "\n"
    # The unique named snapshot cannot be replaced by a later deployment. Keep
    # the stable artifact name as a copy for existing CI/failure collection.
    with immutable.open("x") as output:
        output.write(payload)
    immutable.chmod(0o444)
    (directory / "shutdown_data_check.json").write_text(payload)
    return immutable


def run(work, repo, build, deploy=None):
    work, repo, build = Path(work), Path(repo), Path(build)
    manifest = json.loads((work / "monitor/lifecycle/deployment.json").read_text())
    records = manifest["records"]
    keepers = [record for record in records if record["component"] == "keeper"]
    if not keepers:
        raise ValueError("no actual Keepers")
    for host in sorted({record["host"] for record in keepers}):
        if host in (socket.gethostname(), socket.getfqdn(), "localhost"):
            zone = time.strftime("%z")
        else:
            zone = subprocess.run(["ssh", "-o", "BatchMode=yes", host, "date", "+%z"],
                                  check=True, capture_output=True, text=True, timeout=10).stdout.strip()
        if zone != "+0000":
            raise ValueError("shutdown timestamp evidence requires UTC in every Keeper container")
    for record in keepers:
        conf = json.loads(record["config_bytes"])["chrono_keeper"]["DataStoreInternals"]
        if conf["story_chunk_duration_secs"] != 10 or conf["acceptance_window_secs"] != 15:
            raise ValueError("test must use shipped Keeper chunk and acceptance windows")
    original_generations = {record["config"]: json.loads(Path(record["status_file"]).read_text())["native_proof"]["generation"]
                            for record in records}
    probe_dir = build / "tests/integration/watermark-replay"
    writer = probe_dir / "chronolog-test-shutdown-writer-probe"
    reader = probe_dir / "chronolog-test-rpc-lifecycle-archive-probe"
    chronicle = "ShutdownData_" + str(time.time_ns())
    output = subprocess.run([str(writer), str(work / "conf/default-chrono-client-conf.json"), chronicle],
                            capture_output=True, text=True, timeout=45)
    Path("/tmp/shutdown_events.log").write_text(output.stdout + output.stderr)
    print(output.stdout, end="", flush=True)
    if output.returncode or "CLIENT_NATIVE_CLEANUP 1" not in output.stdout:
        raise ValueError("public retained writer failed or native work remained")
    identities = [line for line in output.stdout.splitlines() if line.startswith("EVENT_ID ")]
    events = [tuple(int(value) for value in line.split()[1:4]) for line in identities]
    if len(events) != 8 or len(set(events)) != 8:
        raise ValueError("writer did not acknowledge eight distinct identities")
    release = [int(line.split()[1]) for line in output.stdout.splitlines() if line.startswith("RELEASE_START ")]
    if len(release) != 1:
        raise ValueError("missing release-entry timestamp")
    identity_file = Path("/tmp/shutdown_events.ids")
    identity_file.write_text("\n".join(identities) + "\n")
    command = [str(reader), str(work / "output"), chronicle, "retained", str(identity_file)]
    before = subprocess.run(command, capture_output=True, text=True, timeout=15)
    print("Before stop:", before.stdout, flush=True)
    if before.returncode != 5 or "ARCHIVE 0 0 0 1 1" not in before.stdout:
        raise ValueError("expected successful empty archive read before shutdown")
    stop_started = time.time_ns()
    controller = Path(deploy) if deploy is not None else repo / "tools/deploy/single_user_deploy.sh"
    if managed_stop(work, controller, "/tmp/shutdown_stop.log"):
        raise ValueError("managed stop did not complete within the original 240s guard")
    stop_finished = time.time_ns()
    for record in records:
        value = json.loads(Path(record["status_file"]).read_text())
        proof = value.get("native_proof", {})
        for key in ("pid", "uid", "host", "executable", "config", "start_ticks", "boot_id"):
            if value.get(key) != record.get(key):
                raise ValueError("stopped native identity differs from original launch")
        if value.get("deployment_id") != manifest["id"] or proof.get("generation") != original_generations[record["config"]]:
            raise ValueError("native generation was replaced during shutdown")
        if value.get("state", "").lower() != "stopped" or value.get("status") != 0:
            raise ValueError("service did not reach normal Stopped")
        if not all(proof.get(key) is True for key in ("complete", "same_primary", "abt_uninitialized")):
            raise ValueError("missing actual original-primary physical retirement")
        if proof.get("application_handles") != 0:
            raise ValueError("native application handles remain")
    boundary = unsealed_until(events, release[0], 10, 15)
    observations = []
    for record in keepers:
        conf = json.loads(record["config_bytes"])["chrono_keeper"]
        log = Path(conf["Monitoring"]["monitor"]["file"])
        paths = sorted(log.parent.glob(log.stem + "*.log"))
        logs = [(path, path.read_text(errors="replace")) for path in paths]
        selected = keeper_shutdown_observation(
            record, logs, chronicle, boundary, stop_started, stop_finished,
            original_generations[record["config"]], manifest["id"])
        if selected:
            observations.append(selected)
    if not observations:
        raise ValueError("no actual Keeper recorded the retained test story")
    after = subprocess.run(command, capture_output=True, text=True, timeout=15)
    print("After stop:", after.stdout, flush=True)
    if after.returncode or "ARCHIVE 0 8 1 1 1" not in after.stdout:
        raise ValueError("archive must contain all eight acknowledged identities and payloads exactly once")
    summary = dict(chronicle=chronicle, events=events, release_started=release[0],
                   unsealed_until=boundary, stop_started=stop_started, stop_finished=stop_finished,
                   holders=len(observations), holder_observations=observations, before_archive_count=0,
                   after_archive_count=8, exact=True, native_stopped=len(records))
    evidence = persist_summary(summary)
    print("Shutdown evidence:", evidence, flush=True)
    # The outer pipeline retains stdout even after it removes the containers.
    print("SHUTDOWN_EVIDENCE", evidence.read_text().strip(), flush=True)
    print("PASS: eight events unsealed when Keeper shutdown began, archived exactly once; all native owners retired", flush=True)


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        raise SystemExit("usage: rpc_shutdown_data_check.py WORK REPO BUILD [DEPLOY_SCRIPT]")
    run(*sys.argv[1:])
