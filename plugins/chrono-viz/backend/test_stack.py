import concurrent.futures
import json
import os
import signal
import socket
import subprocess
import time
import uuid

import chronolog as cl
import requests


def test_real_stack_query_tail_and_failed_keeper():
    python = os.sys.executable
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    env = dict(os.environ, CHRONOLOG_CATALOG=os.environ["CHRONOLOG_TEST_VISOR"],
               CHRONOLOG_PLAYER=os.environ["CHRONOLOG_TEST_PLAYER"])
    server = subprocess.Popen([python, "-m", "uvicorn", "chronolog_viz:app", "--host", "127.0.0.1",
                               "--port", str(port)], env=env)
    c = cl.connect(env["CHRONOLOG_CATALOG"], env["CHRONOLOG_PLAYER"], timeout=5)
    name = "viz-" + uuid.uuid4().hex
    chronicle = c.create_chronicle(name, timeout=5)
    story = c.create_story(chronicle, "events", timeout=5)
    writer = c.acquire(story, "viz-test", timeout=5)
    try:
        for _ in range(100):
            try:
                response = requests.get(base + "/health", timeout=3)
                if response.status_code == 200:
                    break
            except requests.ConnectionError:
                pass
            time.sleep(.05)
        else:
            raise AssertionError("backend health did not become ready")
        assert requests.get(base + "/stories", timeout=5).json()["chronicles"] == [name]
        assert requests.get(base + "/stories", params={"chronicle": name}, timeout=5).json()["stories"] == ["events"]
        first = writer.append(b'{"temperature":23.5}', content_type="application/json",
                              attributes={"host.name": "dragon"}, timeout=5)
        second = writer.append(b'{"temperature":24}', content_type="application/json", timeout=5)
        assert first.acked and second.acked
        body = dict(chronicle=name, story="events", from_ns=first.hlc.physical_ns,
                    to_ns=second.hlc.physical_ns + 1, fields=["temperature"])
        for _ in range(50):
            result = requests.post(base + "/query", json=body, timeout=8)
            assert result.status_code == 200, result.text
            data = result.json()
            if data["meta"]["complete"]:
                break
            time.sleep(.05)
        assert data["meta"]["complete"] is True
        assert [row[2] for row in data["rows"]] == [23.5, 24]
        assert data["rows"][0][-1] == {"host.name": "dragon"}
        limited = requests.post(base + "/query", json={**body, "limit": 1}, timeout=8).json()
        assert len(limited["rows"]) == 1 and limited["meta"]["limited"]
        assert limited["meta"]["complete"] is None
        assert requests.post(base + "/query", json={**body, "axis": "physical"}, timeout=5).status_code == 400
        assert requests.get(base + "/tail", params={"chronicle": name, "story": "events", "after": "bad"}, timeout=5).status_code == 400
        with requests.get(base + "/tail", params={"chronicle": name, "story": "events"},
                          stream=True, timeout=(3, 5)) as response:
            assert response.status_code == 200
            def receive():
                token = None
                for line in response.iter_lines(chunk_size=1):
                    if line.startswith(b"id: "):
                        token = line[4:].decode()
                    if line.startswith(b"data: "):
                        return token, json.loads(line[6:])
                raise AssertionError("Tail closed without an event")
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(receive)
                time.sleep(.1)
                third = writer.append(b'{"temperature":25}', content_type="application/json", timeout=5)
                token, event = pending.result(timeout=6)
                assert event["event_id"] == dict(story_id=third.event_id.story_id, writer_id=third.event_id.writer_id,
                                               incarnation=third.event_id.incarnation, sequence=third.event_id.sequence)
        # Disconnect must release the blocked SDK pull; a resumed stream stays exclusive.
        with requests.get(base + "/tail", params={"chronicle": name, "story": "events", "after": token},
                          stream=True, timeout=(3, 5)) as response:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(receive)
                fourth = writer.append(b'{"temperature":26}', content_type="application/json", timeout=5)
                _, event = pending.result(timeout=6)
                assert event["event_id"]["sequence"] == fourth.event_id.sequence
        os.kill(int(os.environ["CHRONOLOG_TEST_KEEPER_PID"]), signal.SIGKILL)
        failed = requests.post(base + "/query", json=body, timeout=8)
        assert failed.status_code == 200, failed.text
        assert failed.json()["meta"]["complete"] is False
        assert failed.json()["meta"]["reason"] == "SOURCE_FAILED"
        assert failed.json()["meta"]["laggards"]
        server.terminate()
        server.wait(timeout=5)
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=2)
