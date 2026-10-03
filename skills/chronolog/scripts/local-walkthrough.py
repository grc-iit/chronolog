import asyncio
import json
import os
from pathlib import Path
import random
import shlex
import shutil
import socket
import subprocess
import tempfile
import time

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def require(condition, message):
    if not condition:
        raise AssertionError('FAIL ' + message)


async def main():
    root = Path(__file__).resolve().parents[3]
    prefix = root / 'build/smoke-venv'
    with tempfile.TemporaryDirectory(prefix='local3-', dir=root / 'build') as scratch, \
            tempfile.TemporaryDirectory(prefix='local3-archive-') as archive:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('CHRONOLOG_', 'PYTHON', 'LD_'))}
        env.update(PATH=str(prefix / 'bin') + ':' + os.environ['PATH'],
                   CHRONOLOG_HOME=str(Path(scratch) / 'registry'), CHRONOLOG_LOCAL_ROOT=archive)

        def command(args, *, environment=None, ok=True, timeout=60):
            result = subprocess.run(args, env=environment or env, cwd=scratch, text=True,
                                    capture_output=True, timeout=timeout)
            require((result.returncode == 0) == ok, 'command ' + str(args) + ': ' + result.stdout + result.stderr)
            print('PASS command ' + ' '.join(args[:4]))
            return result.stdout if ok else result.stderr

        def cli(*args, **kwargs):
            return json.loads(command(['chronolog', *args], **kwargs))

        base = None
        for _ in range(3):
            candidate = 10000 + random.randrange(4399) * 5
            sockets = []
            try:
                for offset in range(10):
                    sock = socket.socket()
                    sockets.append(sock)
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    sock.bind(('127.0.0.1', candidate + offset))
                base = candidate
                break
            except OSError:
                pass
            finally:
                for sock in sockets:
                    sock.close()
        require(base is not None, 'non-default test port blocks')
        doctor = cli('doctor')
        require(all(Path(path) == prefix / 'bin' / ('chrono_' + role)
                    for role, path in doctor['binaries'].items()), 'installed server discovery')
        require(cli('ls') == [], 'empty isolated registry')
        wal = str(Path(env['CHRONOLOG_HOME']) / 'instances/default/keeper/wal')
        ready = None

        async def attachments(expected):
            deadline = time.monotonic() + 30
            while True:
                state = cli('status', 'default')
                require(state['state'] == 'ready', 'ready while supervisor publishes leases')
                if state['attach']['count'] == expected:
                    return
                require(time.monotonic() < deadline, 'published MCP attachment count')
                await asyncio.sleep(0.1)

        async def mcp_session(server_command, server_args, environment, *, write=False, leased=True):
            parameters = StdioServerParameters(command=server_command, args=server_args, env=environment)
            async with stdio_client(parameters) as (read, send), ClientSession(read, send) as session:
                await asyncio.wait_for(session.initialize(), 30)
                tools = (await asyncio.wait_for(session.list_tools(), 10)).tools
                require({tool.name for tool in tools} == {'context_open', 'context_remember', 'context_recall',
                    'context_latest', 'context_follow', 'context_reconcile', 'context_checkpoint',
                    'context_close', 'context_list', 'context_status'}, 'existing ten MCP tools')
                await attachments(1 if leased else 0)
                if leased:
                    refusal = command(['chronolog', 'down', 'default'], ok=False)
                    require('foreign attach leases' in refusal, 'down refuses a live MCP lease')

                async def tool(name, /, **arguments):
                    result = await asyncio.wait_for(session.call_tool(name, arguments), 15)
                    require(not result.is_error, 'MCP ' + name + ': ' + str(result))
                    return json.loads(result.content[0].text)

                opened = await tool('context_open', name='local3-notes', create=write,
                                    access='read_write' if write else 'read_only')
                require(opened['state'] == 'READY', 'context opened')
                handle = opened['session_handle']
                if write:
                    for index, content in enumerate(('first memory', 'second memory')):
                        remembered = await tool('context_remember', session_handle=handle,
                                                operation_id='local3-' + str(index), content=content)
                        require(remembered['stored'] == 'durable', 'DURABLE memory')
                recalled = None
                for _ in range(30):
                    recalled = await tool('context_recall', session_handle=handle)
                    if recalled['answer_complete']:
                        break
                    await asyncio.sleep(0.1)
                require(recalled['answer_complete'] and [event['content']['data'] for event in recalled['events']]
                        == ['first memory', 'second memory'], 'complete memory recall')
                events = recalled['events']
                ranged = await tool('context_recall', session_handle=handle, start=events[0]['at'], end=events[1]['at'])
                require(ranged['answer_complete'] and len(ranged['events']) == 1, 'half-open time range')
                latest = await tool('context_latest', session_handle=handle, n=1)
                require(latest['selection_complete'] and latest['as_of_token'] and
                        latest['events'][0]['content']['data'] == 'second memory', 'latest certified selection')
                followed = await tool('context_follow', subscriptions=[{'session_handle': handle, 'from': 'beginning'}],
                                      timeout_s=1)
                require(not followed['answer_complete'] and len(followed['pages'][0]['events']) == 2 and
                        followed['pages'][0]['next_follow_token'], 'live follow has no range completeness claim')
                closed = await tool('context_close', session_handle=handle)
                if write:
                    require(closed['release_committed'], 'writer clean close')
            await attachments(0)
            print('PASS MCP lease, complete recall, time range, latest and follow')

        try:
            ready = cli('up', 'default', '--port-base', str(base), '--wal-dir', wal,
                        '--local-root', archive, '--budget-bytes', '200000000000')
            require(ready['state'] == 'ready' and ready['probe'] == 'rpc', 'ready with real RPC probe')
            require(ready['paths']['wal_dir'] == wal and ready['tiers'][0]['root'] == archive and
                    ready['tiers'][0]['budget_bytes'] == 200000000000, 'PI placement flags')
            require(ready['policy']['on_last_detach'] == 'keep', 'default policy keeps running')
            require(cli('ls')[0]['id'] == ready['id'], 'managed instance discovery')
            require(cli('status', 'default', '--probe')['state'] == 'ready', 'status RPC probe')
            exports = command(['chronolog', 'env', 'default'])
            exported = dict(shlex.split(line.removeprefix('export '))[0].split('=', 1)
                            for line in exports.splitlines())
            require(exported['CHRONOLOG_CATALOG'] == ready['endpoints']['catalog'] and
                    exported['CHRONOLOG_INSTANCE'] == ready['id'] and
                    exported['CHRONOLOG_MCP_LOCK_DIR'] == str(Path(env['CHRONOLOG_HOME']) / 'locks'), 'env exports')
            override = dict(env, CHRONOLOG_CATALOG=ready['endpoints']['catalog'],
                            CHRONOLOG_PLAYER=ready['endpoints']['player'])
            overridden = json.loads(command(['chronolog', 'run', '--up', 'default', '--', str(prefix / 'bin/python'),
                '-c', 'import json, os; print(json.dumps(dict(instance=os.environ["CHRONOLOG_INSTANCE"], catalog=os.environ["CHRONOLOG_CATALOG"])))'],
                environment=override))
            require(overridden['instance'] == 'environment' and overridden['catalog'] == override['CHRONOLOG_CATALOG'],
                    'explicit Catalog overrides registry selection')
            await mcp_session('chronolog', ['run', '--up', 'default', '--', 'chronolog-mcp', '--identity',
                              'agent-memory/main', '--chronicle', 'agent-memory'], env, write=True)
            kept = cli('status', 'default')
            require(kept['state'] == 'ready' and kept['attach']['count'] == 0, 'last detach keeps default ready')
            for role, marker in (('supervisor', 'start visor'), ('visor', 'catalog ready')):
                require(marker in command(['chronolog', 'logs', 'default', '--service', role, '--lines', '40']), 'diagnostic logs')
            require(cli('doctor') == doctor, 'diagnostic doctor')
            require(cli('down', 'default', timeout=210)['state'] == 'stopped', 'ordered stop preserves data')
            require(cli('ls')[0]['state'] == 'stopped', 'stopped discovery')
            again = cli('up', 'default')
            require(again['id'] == ready['id'] and again['endpoints'] == ready['endpoints'], 'restart identity and endpoints')
            manifest = json.loads((root / '.claude-plugin/marketplace.json').read_text())['plugins'][0]['mcpServers']['chronolog']
            marketplace_env = dict(env, CHRONOLOG_MCP_IDENTITY='agent-memory/main', CHRONOLOG_CHRONICLE='agent-memory',
                                   CHRONOLOG_MCP_LOCK_DIR=str(Path(env['CHRONOLOG_HOME']) / 'locks'))
            await mcp_session(manifest['command'], manifest['args'], marketplace_env)
            uvx = shutil.which('uvx')
            require(uvx is not None, 'real uvx for marketplace fallback')
            fallback = Path(scratch) / 'fallback'
            fallback.mkdir()
            (fallback / 'uvx').symlink_to(uvx)
            fallback_env = dict(marketplace_env, PATH=str(fallback) + ':/usr/bin:/bin',
                                CHRONOLOG_CATALOG=ready['endpoints']['catalog'], CHRONOLOG_PLAYER=ready['endpoints']['player'],
                                UV_FIND_LINKS=str(root / 'build/smoke/wheels'))
            await mcp_session(manifest['command'], manifest['args'], fallback_env, leased=False)
            require(cli('down', 'default', '--purge', timeout=210)['state'] == 'stopped', 'throwaway default purge')
            ready = None
            scratch_ready = cli('up', 'scratch', '--ephemeral', '--port-base', str(base), '--idle-grace-s', '1')
            require(scratch_ready['policy']['on_last_detach'] == 'stop', 'ephemeral stop policy')
            deadline = time.monotonic() + 30
            while cli('status', 'scratch')['state'] != 'stopped':
                require(time.monotonic() < deadline, 'ephemeral idle stop')
                await asyncio.sleep(0.1)
            require(cli('down', 'scratch', '--purge', timeout=210)['state'] == 'stopped', 'ephemeral purge')
            require(cli('ls') == [], 'walkthrough registry cleanup')
            print('PASS LOCAL-3 walkthrough: all documented local commands, persistent memory, ephemeral cleanup, both marketplace paths')
        finally:
            for name in ('default', 'scratch'):
                command(['chronolog', 'down', name, '--purge'], ok=False if not
                        (Path(env['CHRONOLOG_HOME']) / 'instances' / name).exists() else True, timeout=210)


if __name__ == '__main__':
    asyncio.run(asyncio.wait_for(main(), 420))
