import json
import time
import urllib.request

import chronolog as cl

with cl.connect("127.0.0.1:50051", "127.0.0.1:50054", timeout=5) as client:
    try:
        chronicle = client.create_chronicle("viz-example", timeout=5)
    except cl.AlreadyExists:
        chronicle = client.chronicle("viz-example", timeout=5)
    try:
        story = client.create_story(chronicle, "temperature", timeout=5)
    except cl.AlreadyExists:
        story = next(s for s in client.list_stories(chronicle, timeout=5) if s.name == "temperature")
    with client.acquire(story, "viz-example", timeout=5) as writer:
        receipt = writer.append(b'{"temperature":23.5}', content_type="application/json",
                                attributes={"host.name": "example", "unit": "Cel"}, timeout=5)
        assert receipt.acked
    body = dict(chronicle=chronicle.name, story=story.name, from_ns=receipt.hlc.physical_ns,
                to_ns=time.time_ns(), fields=["temperature"])
    request = urllib.request.Request("http://127.0.0.1:8087/query", json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        print(response.read().decode())
