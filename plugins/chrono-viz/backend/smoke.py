import json
import time
import uuid

import chronolog as cl
import requests

c = cl.connect("127.0.0.1:50051", "127.0.0.1:50054", timeout=5)
chronicle = c.create_chronicle("viz-smoke-" + uuid.uuid4().hex, timeout=5)
story = c.create_story(chronicle, "events", timeout=5)
with c.acquire(story, "smoke", timeout=5) as writer:
    receipts = writer.append_batch([cl.Envelope(json.dumps({"value": x}).encode(), "application/json",
                                    {"host.name": "dragon"}) for x in range(3)], timeout=5)
    assert all(r.acked for r in receipts)
    body = dict(chronicle=chronicle.name, story=story.name, from_ns=receipts[0].hlc.physical_ns,
                to_ns=receipts[-1].hlc.physical_ns + 1, fields=["value"])
    auth = ("admin", "chronolog-viz-password")
    for _ in range(100):
        try:
            response = requests.get("http://127.0.0.1:3000/api/health", timeout=3)
            if response.status_code == 200 and response.json().get("database") == "ok":
                break
        except requests.RequestException:
            pass
        time.sleep(.2)
    else:
        raise AssertionError("Grafana did not become ready")
    # /api/health answers when Grafana listens, before it has provisioned the datasource, and the first
    # authenticated requests can stall behind its own SQLite startup writes. Poll the provisioned
    # datasource, the state the test depends on, with short per-request timeouts.
    def wait_for(url, ready, seconds=60):
        end, last = time.monotonic() + seconds, None
        while time.monotonic() < end:
            try:
                response = requests.get(url, auth=auth, timeout=3)
                if ready(response):
                    return response
                last = response.text
            except requests.RequestException as error:
                last = repr(error)
            time.sleep(.2)
        raise AssertionError(url + ": " + str(last))

    datasource = wait_for("http://127.0.0.1:3000/api/datasources/uid/chronolog-viz", lambda r: r.status_code == 200)
    assert datasource.json()["type"] == "chronolog-viz-datasource"
    wait_for("http://127.0.0.1:3000/api/plugins/chronolog-viz-datasource/settings", lambda r: r.status_code == 200)
    for base in ("http://127.0.0.1:8087", "http://127.0.0.1:3000/api/datasources/proxy/uid/chronolog-viz"):
        health = requests.get(base + "/health", auth=auth, timeout=8)
        assert health.status_code == 200 and health.json()["status"] == "healthy", health.text
        for _ in range(30):
            response = requests.post(base + "/query", json=body, auth=auth, timeout=8)
            assert response.status_code == 200, response.text
            result = response.json()
            if result["meta"]["complete"]:
                break
            time.sleep(.1)
        assert result["meta"]["complete"] is True, result
        assert [row[2] for row in result["rows"]] == [0, 1, 2], result
        print("PASS ChronoLog query and datasource health " + base, flush=True)
print("PASS unsigned ChronoLog Grafana plugin settings", flush=True)
