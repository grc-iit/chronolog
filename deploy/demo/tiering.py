#!/usr/bin/env python3
"""ChronoLog tiering demo: a single-node instance moves its archive to a slower tier, reports it in
`chronolog status`, and a read over an unavailable tier ends SOURCE_FAILED (never LOST) until the tier returns,
with identical EventIds and HLCs throughout. The last line is PASS tiering or FAIL <reason>.

Run with an install prefix's bin directory on PATH (chronolog and a Python with the chronolog module). All three
directories must be empty or absent; --slow-root belongs on a slower file system and is removed on PASS:
  python deploy/demo/tiering.py --wal-dir DIR --local-root DIR --slow-root DIR [--instance NAME]
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

import chronolog as cl

# Age-driven migration for a demo: windows 2 s long, moved once 3 s old (I13.13, I13.16). migrate_after_s must
# exceed compact_min_age_secs + compact_scan_interval_secs, so compaction is off and its age is 0.
OVERRIDES = {
    'keeper': {'story_chunk_duration_secs': 2, 'archive_visibility_delay_secs': 2},
    'grapher': {'migrate_enabled': True, 'migrate_after_s': 3, 'compact_enabled': False, 'compact_min_age_secs': 0,
                'compact_scan_interval_secs': 1, 'scrub_interval_s': 1, 'scrub_slow_tiers': True,
                'tier_probe_interval_ms': 500},
    'player': {'tier_probe_interval_ms': 500}}
EVENTS, PAYLOAD = 64, 64 * 1024


def cli(*args, timeout=60, quiet=False):
    result = subprocess.run(['chronolog', *args], capture_output=True, text=True, timeout=timeout)
    assert result.returncode == 0, f'FAIL chronolog {" ".join(args)}: {result.stdout}{result.stderr}'
    data = json.loads(result.stdout)
    if not quiet:
        print(f'chronolog {" ".join(args)}: {json.dumps(data)}', flush=True)
    return data


def tiers(status):
    """name -> the Grapher's view of the tier (known, available, used_bytes) and the launcher's own probe."""
    return {tier['name']: {key: value for key, value in (tier.get('grapher') or {'known': False}).items()
                           if key in ('known', 'available', 'used_bytes', 'reason')} | {'launcher_probe_available': tier.get('available')}
            for tier in status['tiers']}


def wait(label, seconds, condition):
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        value = condition()
        if value:
            print(f'{label} after {time.monotonic() - start:.2f} s', flush=True)
            return value
        time.sleep(0.25)
    raise AssertionError(f'FAIL {label} within {seconds} s')


def read(client, story, receipts):
    end = cl.Hlc(receipts[-1][1].physical_ns, receipts[-1][1].logical + 1)
    stream = client.read(story, receipts[0][1], end, timeout=30)
    events = [(event.id, event.hlc) for event in stream]
    return events, stream.completion


def no_lost(status):
    """The scrubber may fail to validate files on an unavailable tier (slow_failed); it never records them LOST
    (I13.15). Its per-pass log line carries both counts."""
    scrub = (status.get('tier_migration') or {}).get('scrub')
    log = (Path(status['paths']['logs']) / 'grapher.log').read_text(errors='replace')
    passes = re.findall(r'archive scrub validated=\d+ .*lost=(\d+) .*slow_failed=(\d+)', log)
    assert 'archive scrub found' not in log and all(lost == '0' for lost, _ in passes), 'FAIL the Grapher recorded LOST'
    if scrub is not None:
        assert scrub['lost'] == 0, 'FAIL scrub reports LOST: ' + json.dumps(scrub)
    print(f'LOST check: {len(passes)} scrub passes, lost=0 in every one, most slow_failed='
          f'{max((int(failed) for _, failed in passes), default=0)}, status scrub={json.dumps(scrub)}', flush=True)


def main(args):
    name, slow = args.instance, Path(args.slow_root).resolve()
    if subprocess.run(['chronolog', 'status', name], capture_output=True).returncode == 0:
        cli('down', name, '--purge', timeout=210)
    for directory in (args.wal_dir, args.local_root, slow):
        assert not Path(directory).exists() or not any(Path(directory).iterdir()), f'FAIL {directory} is not empty'
    slow.mkdir(parents=True, exist_ok=True)
    cli('create', name, '--wal-dir', args.wal_dir, '--local-root', args.local_root, '--tier-probe-interval-ms', '500',
        '--overrides', json.dumps(OVERRIDES))
    cli('tier', 'add', name, 'nfs', str(slow), '--rank', '1', '--kind', 'slow')
    start = time.monotonic()
    ready = cli('up', name, quiet=True)
    print(f'ready seconds={time.monotonic() - start:.3f} tiers={json.dumps(tiers(ready))}', flush=True)
    client = cl.connect(ready['endpoints']['catalog'], ready['endpoints']['player'], timeout=10)
    story = client.create_story(client.create_chronicle(f'tiering-{time.time_ns()}'), 'measurements')
    receipts = []
    with client.acquire(story, 'sensor') as writer:
        for batch in range(8):
            payloads = [bytes([batch * 8 + i]) * PAYLOAD for i in range(EVENTS // 8)]
            for result in writer.append_batch(payloads, timeout=10):
                assert isinstance(result, cl.AppendResult) and result.acked, f'FAIL durable append: {result!r} {getattr(result, "rejection", None)!r}'
                receipts.append((result.event_id, result.hlc))
            time.sleep(0.75)
    print(f'wrote {len(receipts)} events of {PAYLOAD} bytes over {receipts[-1][1].physical_ns - receipts[0][1].physical_ns} ns', flush=True)

    def migrated():
        view = tiers(cli('status', name, quiet=True))
        nfs, local = view['nfs'], view['local']
        return view if nfs.get('known') and nfs.get('used_bytes', 0) > 0 and local.get('used_bytes') == 0 else None
    print('tiers after migration: ' + json.dumps(wait('every archive file on nfs', 120, migrated)), flush=True)

    events, completion = read(client, story, receipts)
    print(f'read with nfs available: {len(events)} events, complete={completion.complete} reason={completion.reason.name}', flush=True)
    assert completion.complete and events == receipts, 'FAIL read after migration'

    os.chmod(slow, 0)
    try:
        def failed():
            events, completion = read(client, story, receipts)
            return (events, completion) if completion.reason == cl.IncompleteReason.SOURCE_FAILED else None
        events, completion = wait('read over the unavailable tier ends SOURCE_FAILED', 60, failed)
        print(f'read with nfs unavailable: {len(events)} events, complete={completion.complete} reason={completion.reason.name}', flush=True)
        assert not completion.complete, 'FAIL SOURCE_FAILED read claims completeness'

        def reported():
            status = cli('status', name, quiet=True)
            nfs = tiers(status)['nfs']
            return status if nfs.get('available') is False and nfs['launcher_probe_available'] is False else None
        down = wait('chronolog status reports nfs unavailable', 30, reported)
        print('tiers while unavailable: ' + json.dumps(tiers(down)), flush=True)
        no_lost(down)
    finally:
        os.chmod(slow, 0o700)

    def recovered():
        events, completion = read(client, story, receipts)
        return (events, completion) if completion.complete else None
    events, completion = wait('read after the tier returns is complete', 60, recovered)
    print(f'read with nfs restored: {len(events)} events, complete={completion.complete}', flush=True)
    assert events == receipts, 'FAIL EventIds or HLCs changed across the outage'
    final = wait('chronolog status reports nfs available again', 30,
                 lambda: (lambda status: status if tiers(status)['nfs'].get('available') else None)(cli('status', name, quiet=True)))
    print('tiers after recovery: ' + json.dumps(tiers(final)), flush=True)
    no_lost(final)
    stopped = cli('down', name, timeout=210)
    assert stopped['state'] == 'stopped', 'FAIL ordered stop'
    shutil.rmtree(slow)
    print('PASS tiering', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--wal-dir', required=True)
    parser.add_argument('--local-root', required=True)
    parser.add_argument('--slow-root', required=True)
    parser.add_argument('--instance', default='tiering')
    options = parser.parse_args()
    try:
        main(options)
    except BaseException as error:
        print('FAIL ' + repr(error), flush=True)
        if Path(options.slow_root).exists():
            os.chmod(options.slow_root, 0o700)
        try:
            subprocess.run(['chronolog', 'down', options.instance], capture_output=True, timeout=210)
        except BaseException as cleanup:
            print('Cleanup: ' + repr(cleanup), flush=True)
        raise
