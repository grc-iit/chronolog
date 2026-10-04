import asyncio
from datetime import datetime, timezone
import os
import subprocess
import sys

import chronolog as cl
import pytest

from _mcp import Mcp, VISOR, launcher, unique
from chronomcp.server import acceptance_bound, chronolog_home
from chronomcp.store import Launcher, token


def test_time_parser_preserves_nanoseconds():
    assert acceptance_bound('1970-01-01T01:00:00.123456789+01:00', 'since') == cl.Hlc(123456789, 0)
    assert acceptance_bound('1970-01-01t00:00:00z', 'until') == cl.Hlc(0, 0)
    assert acceptance_bound(2**63 - 1, 'until') == cl.Hlc(2**63 - 1, 0)
    assert acceptance_bound(-1, 'since') == cl.Hlc(-1, 0)
    for value in (True, 1.5, 2**63, -(2**63)-1, '123', '2026-10-03T12:00:00',
                  '2026-02-30T00:00:00Z', '2026-10-03T00:00:00.1234567890Z', '1970-01-01T00:00:00+00:60'):
        with pytest.raises(ValueError):
            acceptance_bound(value, 'since')


def test_home_resolution_ignores_runtime(monkeypatch):
    monkeypatch.delenv('CHRONOLOG_HOME', raising=False)
    monkeypatch.setenv('XDG_STATE_HOME', '/state')
    monkeypatch.setenv('XDG_RUNTIME_DIR', '/runtime-a')
    assert chronolog_home() == '/state/chronolog'
    monkeypatch.setenv('XDG_RUNTIME_DIR', '/runtime-b')
    assert chronolog_home() == '/state/chronolog'
    monkeypatch.delenv('XDG_STATE_HOME')
    assert chronolog_home() == os.path.expanduser('~/.local/state/chronolog')
    monkeypatch.setenv('CHRONOLOG_HOME', '/explicit')
    assert chronolog_home() == '/explicit'


def test_canonical_lock_and_legacy_key(tmp_path):
    lock = tmp_path / 'locks'
    first = Launcher('planner', 'one', 'host', lock, 'localhost:50051')
    try:
        assert first.held
        for catalog in ('127.0.0.1:50051', 'LOCALHOST.:050051', 'localhost:50051'):
            second = Launcher('planner', 'two', 'host', lock, catalog)
            try:
                assert not second.held
            finally:
                second.release()
        # A pre-upgrade launcher using the original raw key must also be excluded.
        code = "import fcntl,sys; f=open(sys.argv[1],'a+'); fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)"
        old = subprocess.run([sys.executable, '-c', code, first.lock_id.removeprefix('flock:')],
                             capture_output=True, timeout=5)
        assert old.returncode != 0 and 'BlockingIOError' in old.stderr.decode()
    finally:
        first.release()
    second = Launcher('planner', 'two', 'host', lock, '127.0.0.1:50051')
    try:
        assert second.held
    finally:
        second.release()


@pytest.mark.skipif(not VISOR, reason='real stack required')
def test_acceptance_ranges_and_server_budget(tmp_path):
    async def run():
        args = launcher(str(tmp_path), unique('planner'), unique('local2'), '--max-json-bytes', '14336')
        async with Mcp(args) as mcp:
            handle = (await mcp.call('context_open', name='notes', create=True))['session_handle']
            events = []
            for i in range(3):
                memory = await mcp.call('context_remember', session_handle=handle,
                                        operation_id=str(i), content=str(i))
                assert memory['stored'] == 'durable', memory
                events.append(memory['receipt'])
            start = int(events[0]['hlc'][0])
            end = int(events[2]['hlc'][0])
            assert start < end
            story = (await mcp.call('context_status', session_handle=handle))['sessions'][0]['context']['story_id']
            lower = token('a1', [story, str(start), 0])
            upper = token('a1', [story, str(end), 0])
            by_token = await mcp.call('context_recall', session_handle=handle, start=lower, end=upper)
            seconds, ns = divmod(start, 1_000_000_000)
            rfc = datetime.fromtimestamp(seconds, timezone.utc).strftime('%Y-%m-%dT%H:%M:%S') + f'.{ns:09d}Z'
            for since in (start, rfc):
                by_time = await mcp.call('context_recall', session_handle=handle, since=since, until=end)
                assert by_time['events'] == by_token['events']
                assert by_time['range'] == by_token['range']
                assert by_time['answer_complete'] == by_token['answer_complete']
            by_token = await mcp.call('context_latest', session_handle=handle, before=upper)
            by_time = await mcp.call('context_latest', session_handle=handle, until=end)
            assert by_time['events'] == by_token['events']
            assert by_time['as_of'] == by_token['as_of']
            for tool, kwargs in [('context_recall', {'since': start, 'start': lower}),
                                 ('context_recall', {'until': end, 'cursor': 'bad'}),
                                 ('context_recall', {'since': end, 'until': start}),
                                 ('context_latest', {'until': end, 'before': upper})]:
                assert (await mcp.raw(tool, session_handle=handle, **kwargs)).is_error
            tools = (await mcp.session.list_tools()).tools
            for tool in tools:
                if tool.name in ('context_recall', 'context_latest', 'context_follow'):
                    assert tool.input_schema['properties']['max_json_bytes']['default'] == 14336
            for i in range(6):
                assert (await mcp.call('context_remember', session_handle=handle,
                                      operation_id=f'large-{i}', content='x' * 3000))['stored'] == 'durable'
            budgeted = await mcp.call('context_recall', session_handle=handle)
            assert len(budgeted['events']) < 9 and budgeted['has_more'], budgeted
            assert len(str(budgeted).encode()) < 16384
            override = await mcp.call('context_recall', session_handle=handle, max_json_bytes=65536)
            assert len(override['events']) == 9, override
    asyncio.run(asyncio.wait_for(run(), timeout=50))


@pytest.mark.skipif(not VISOR, reason='real stack required')
def test_servers_share_lock_across_runtime_directories(tmp_path, monkeypatch):
    async def run():
        monkeypatch.setenv('CHRONOLOG_HOME', str(tmp_path / 'state'))
        monkeypatch.delenv('CHRONOLOG_MCP_LOCK_DIR', raising=False)
        args = launcher(str(tmp_path), unique('planner'), unique('runtime'))
        idx = args.index('--lock-dir')
        del args[idx:idx + 2]
        monkeypatch.setenv('XDG_RUNTIME_DIR', str(tmp_path / 'runtime-a'))
        async with Mcp(args) as first:
            handle = (await first.call('context_open', name='notes', create=True))['session_handle']
            monkeypatch.setenv('XDG_RUNTIME_DIR', str(tmp_path / 'runtime-b'))
            async with Mcp(args) as second:
                status = await second.call('context_status')
                assert status['checkpoint_store']['state'] == 'slot_locked', status
                refused = await second.call('context_open', name='notes')
                assert refused['session_handle'] is None, refused
                reader = await second.call('context_open', name='notes', access='read_only')
                assert reader['session_handle']
            assert (await first.call('context_remember', session_handle=handle,
                                     operation_id='one', content='owner'))['stored'] == 'durable'
    asyncio.run(asyncio.wait_for(run(), timeout=50))
