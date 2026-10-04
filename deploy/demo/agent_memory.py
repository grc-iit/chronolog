#!/usr/bin/env python3
"""ChronoLog agent memory demo: five durable remembers through chronolog-mcp, complete and windowed recall,
and recovery from a SIGKILLed supervisor and a SIGKILLed Keeper with identical EventIds and HLCs.
Every step asserts what it claims; the last line is PASS agent-memory or FAIL <reason>.

Run with an install prefix's bin directory on PATH (chronolog, chronolog-mcp, and a Python with mcp):
  python deploy/demo/agent_memory.py --wal-dir DIR --local-root DIR
"""
import argparse
import asyncio
from contextlib import asynccontextmanager
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

DECISIONS = ['Use durable acknowledgements for decisions.', 'Keep one stable identity per writer.',
             'Check Completion before inferring absence.', 'Place WAL on home NVMe and archive on root NVMe.',
             'Reuse operation IDs when retrying uncertain writes.']
DEFAULT_MARKETPLACE = Path(__file__).resolve().parents[2] / '.claude-plugin/marketplace.json'
os.environ.update(CHRONOLOG_CHRONICLE='demo1', CHRONOLOG_MCP_IDENTITY='demo1/planner')
LAUNCH = {}


def cli(*args, timeout=45, quiet=False):
    result = subprocess.run(['chronolog', *args], capture_output=True, text=True, timeout=timeout)
    assert result.returncode == 0, 'FAIL CLI ' + str(args) + ': ' + result.stdout + result.stderr
    data = json.loads(result.stdout)
    if not quiet:
        print('chronolog ' + ' '.join(args) + ': ' + json.dumps(data), flush=True)
    return data


@asynccontextmanager
async def session():
    params = StdioServerParameters(command=LAUNCH['command'], args=LAUNCH['args'], env=dict(os.environ))
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as client:
            await client.initialize()
            tools = await client.list_tools()
            assert len(tools.tools) == 12, 'FAIL twelve MCP tools'
            yield client


async def call(client, tool, **args):
    result = await asyncio.wait_for(client.call_tool(tool, args), 30)
    assert not result.is_error, 'FAIL MCP ' + tool + ': ' + str(result)
    return json.loads(result.content[0].text)


def identities(events):
    return [(event['id'], event['hlc']) for event in events]


def check(page, expected, label):
    print(label + ': ' + json.dumps(page), flush=True)
    assert page['answer_complete'] and page['completion']['complete'], 'FAIL ' + label + ' incomplete'
    assert not page['has_more'] and page['next_cursor'] is None, 'FAIL ' + label + ' pagination'
    assert identities(page['events']) == expected, 'FAIL ' + label + ' EventId/HLC mismatch'


async def recall(label, expected):
    async with session() as client:
        opened = await call(client, 'context_open', name='decisions', agent='observer', access='read_only')
        page = await call(client, 'context_recall', session_handle=opened['session_handle'])
        check(page, expected, label)


async def main(args):
    cli('doctor')
    start = time.monotonic()
    ready = cli('up', '--wal-dir', args.wal_dir, '--local-root', args.local_root)
    print(f'ready seconds={time.monotonic() - start:.3f}', flush=True)
    assert ready['state'] == 'ready' and ready['probe'] == 'rpc', 'FAIL readiness'
    assert ready['policy']['on_last_detach'] == 'keep', 'FAIL default keep policy'
    cli('ls')
    receipts = []
    async with session() as client:
        opened = await call(client, 'context_open', name='decisions', create=True, agent='planner')
        for index, decision in enumerate(DECISIONS):
            reply = await call(client, 'context_remember', session_handle=opened['session_handle'],
                               operation_id=f'decision-{index + 1}', content=decision)
            assert reply['stored'] == 'durable' and reply['receipt'] is not None, 'FAIL durable remember'
            receipt = reply['receipt']
            receipts.append((receipt['event_id'], receipt['hlc']))
            print('remember ' + json.dumps(receipt), flush=True)
        await call(client, 'context_close', session_handle=opened['session_handle'])
    assert len({json.dumps(item[0], sort_keys=True) for item in receipts}) == 5, 'FAIL distinct EventIds'
    since = int(receipts[1][1][0])
    until = int(receipts[4][1][0])
    assert int(receipts[0][1][0]) < since < until, 'FAIL window boundaries'
    async with session() as client:
        opened = await call(client, 'context_open', name='decisions', agent='observer', access='read_only')
        handle = opened['session_handle']
        page = await call(client, 'context_recall', session_handle=handle)
        check(page, receipts, 'session 2 complete')
        assert [event['content']['data'] for event in page['events']] == DECISIONS, 'FAIL decision contents'
        page = await call(client, 'context_recall', session_handle=handle, since=since, until=until)
        check(page, receipts[1:4], 'session 2 middle three')
    # Wait only for the SIGKILLed supervisor's kernel-owned sockets to disappear.
    before = cli('status')
    start = time.monotonic()
    os.kill(before['supervisor_pid'], signal.SIGKILL)
    for _ in range(300):
        sockets = []
        try:
            for endpoint in before['endpoints'].values():
                sock = socket.socket()
                sockets.append(sock)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind(('127.0.0.1', int(endpoint.rsplit(':', 1)[1])))
            break
        except OSError:
            await asyncio.sleep(0.01)
        finally:
            for sock in sockets:
                sock.close()
    else:
        raise AssertionError('FAIL supervisor children retain sockets')
    recovered = cli('up')
    print(f'supervisor recovery seconds={time.monotonic() - start:.3f}', flush=True)
    assert recovered['state'] == 'ready' and recovered['endpoints'] == before['endpoints'], 'FAIL restart endpoints'
    await recall('supervisor recovered identical', receipts)
    # Open first so process startup does not consume the interval before automatic service restart.
    async with session() as client:
        opened = await call(client, 'context_open', name='decisions', agent='observer', access='read_only')
        before = cli('status')
        keeper = before['services']['keeper']['pid']
        start = time.monotonic()
        os.kill(keeper, signal.SIGKILL)
        page = await call(client, 'context_recall', session_handle=opened['session_handle'])
        print('immediate Keeper crash Completion: ' + json.dumps(page), flush=True)
        assert page['completion'] is not None, 'FAIL crash recall has no Completion'
        if page['answer_complete']:
            check(page, receipts, 'crash read certified complete')
        else:
            assert not page['completion']['complete'], 'FAIL contradictory incomplete answer'
        for _ in range(300):
            current = cli('status', quiet=True)
            service = current['services']['keeper']
            if current['state'] == 'ready' and service['pid'] != keeper and service['state'] == 'ready':
                break
            await asyncio.sleep(0.1)
        else:
            raise AssertionError('FAIL supervisor did not automatically recover Keeper within 30 seconds')
        print(f'Keeper automatic recovery seconds={time.monotonic() - start:.3f}', flush=True)
        page = await call(client, 'context_recall', session_handle=opened['session_handle'])
        check(page, receipts, 'Keeper recovered identical')
    stopped = cli('down', timeout=210)
    assert stopped['state'] == 'stopped', 'FAIL ordered stop'
    listing = cli('ls')
    assert len(listing) == 1 and listing[0]['state'] == 'stopped', 'FAIL final state'
    print('PASS agent-memory', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--wal-dir', required=True)
    parser.add_argument('--local-root', required=True)
    parser.add_argument('--marketplace', type=Path, default=DEFAULT_MARKETPLACE)
    options = parser.parse_args()
    LAUNCH.update(json.loads(options.marketplace.read_text())['plugins'][0]['mcpServers']['chronolog'])
    try:
        asyncio.run(asyncio.wait_for(main(options), 290))
    except BaseException as error:
        print('FAIL ' + repr(error), flush=True)
        try:
            cli('down', timeout=210)
        except BaseException as cleanup:
            print('Cleanup: ' + repr(cleanup), flush=True)
        raise
