"""Lifecycle gates against real LOCAL-1 supervisors and service binaries."""
import asyncio
import os
from pathlib import Path
import subprocess
import sys

import pytest

from _mcp import Mcp, unique


@pytest.fixture
def local(tmp_path, monkeypatch):
    root = Path(__file__).resolve().parents[3]
    monkeypatch.setenv('CHRONOLOG_HOME', str(tmp_path / 'home'))
    monkeypatch.setenv('CHRONOLOG_BIN_DIR', os.environ.get('CHRONOLOG_BIN_DIR', str(root / 'build/dev')))
    for variable in ('CHRONOLOG_CATALOG', 'CHRONOLOG_PLAYER', 'CHRONOLOG_INSTANCE',
                     'CHRONOLOG_AUTOSTART', 'CHRONOLOG_MCP_LOCK_DIR'):
        monkeypatch.delenv(variable, raising=False)
    monkeypatch.setenv('CHRONOLOG_INSTANCE', 'default')
    # Make the source launcher available to MCP subprocesses as well as this gate.
    monkeypatch.setenv('PYTHONPATH', str(root / 'launcher') + os.pathsep + os.environ.get('PYTHONPATH', ''))
    yield ['--identity', unique('local-owner'), '--chronicle', unique('local-context'), '--timeout', '5']
    for record in (tmp_path / 'home/instances').glob('*/instance.json'):
        result = subprocess.run([sys.executable, '-m', 'chronolog_local.cli', 'down', record.parent.name, '--force'],
                                capture_output=True, text=True, timeout=210)
        assert result.returncode == 0, result.stderr


def test_unbound_lifecycle_and_lease(local):
    async def run():
        async with Mcp(local) as first:
            tools = (await first.session.list_tools()).tools
            assert len(tools) == 12
            for name in ('context_open', 'context_status', 'context_list'):
                result = await first.call(name, **({'name': 'notes'} if name == 'context_open' else {}))
                assert result['store_state'] == 'unbound' and 'instance_control' in result['verdict'], result
            assert (await first.call('instance_list'))['instances'] == []
            missing = await first.raw('instance_control', action='up')
            assert missing.is_error
            up = await first.call('instance_control', action='up', create=True,
                                  on_last_detach='stop', idle_grace_s=300)
            assert up['instance']['state'] == 'ready', up
            assert up['instance']['attach']['count'] == 1, up
            handle = (await first.call('context_open', name='notes', create=True))['session_handle']
            remembered = await first.call('context_remember', session_handle=handle, operation_id='one', content='retained')
            assert remembered['stored'] == 'durable', remembered
            refused = await first.call('instance_control', action='up', name='other', create=True)
            assert not refused['changed'] and 'rebind refused' in refused['verdict'], refused
            async with Mcp(['--identity', unique('reader'), '--timeout', '5']) as second:
                status = await second.call('context_status')
                assert status['instance']['attach']['count'] == 2, status
                listing = await second.call('instance_list', probe=True)
                assert listing['instances'][0]['bound'], listing
                assert (await second.call('instance_control', action='detach'))['instance'] is None
                assert (await second.call('context_list'))['store_state'] == 'unbound'
                assert (await second.call('instance_control', action='attach'))['instance']['state'] == 'ready'
                blocked = await second.raw('instance_control', action='down')
                assert blocked.is_error and 'foreign attach leases' in blocked.content[0].text, blocked
                assert (await second.call('context_status'))['store_state'] == 'unbound'
            detached = await first.call('instance_control', action='detach')
            assert detached['instance'] is None
            await first.call('instance_control', action='attach')
            reopened = await first.call('context_open', name='notes', access='read_only')
            recall = await first.call('context_recall', session_handle=reopened['session_handle'])
            assert any(e['content']['data'] == 'retained' for e in recall['events']), recall
            await first.call('instance_control', action='down')
            listing = await first.call('instance_list')
            assert listing['instances'][0]['state'] == 'stopped', listing
    asyncio.run(asyncio.wait_for(run(), 100))


def test_autostart_and_registry_slot_identity(local, monkeypatch, tmp_path):
    async def run():
        monkeypatch.setenv('CHRONOLOG_AUTOSTART', '1')
        monkeypatch.setenv('CHRONOLOG_INSTANCE', 'default')
        monkeypatch.setenv('XDG_RUNTIME_DIR', str(tmp_path / 'runtime-a'))
        async with Mcp(local) as first:
            status = await first.call('context_status')
            instance = status['instance']
            assert instance['state'] == 'ready' and instance['attach']['count'] == 1, status
            handle = (await first.call('context_open', name='notes', create=True))['session_handle']
            monkeypatch.setenv('CHRONOLOG_INSTANCE', instance['id'])
            monkeypatch.setenv('XDG_RUNTIME_DIR', str(tmp_path / 'runtime-b'))
            async with Mcp(local) as second:
                status = await second.call('context_status')
                assert status['instance']['id'] == instance['id']
                assert status['checkpoint_store']['state'] == 'slot_locked', status
                assert (await second.call('context_open', name='notes'))['session_handle'] is None
            monkeypatch.setenv('CHRONOLOG_CATALOG', instance['endpoints']['catalog'])
            async with Mcp(local) as third:
                status = await third.call('context_status')
                assert status['instance']['id'] == instance['id'], status
                assert status['instance']['attach']['count'] == 2, status
                assert status['checkpoint_store']['state'] == 'slot_locked', status
            assert (await first.call('context_remember', session_handle=handle,
                                     operation_id='owner', content='owner'))['stored'] == 'durable'
            await first.call('instance_control', action='down')
    asyncio.run(asyncio.wait_for(run(), 80))
