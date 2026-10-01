import argparse
import base64
from dataclasses import asdict
import json
import os
import time

from chronolog import Hlc, connect


def _json(value):
    return json.dumps(value, separators=(",", ":"))


def _event(event):
    envelope = event.envelope
    try:
        payload, encoded = envelope.payload.decode("utf-8"), False
    except UnicodeDecodeError:
        payload, encoded = base64.b64encode(envelope.payload).decode("ascii"), True
    return {"id": asdict(event.id), "hlc": asdict(event.hlc),
            "physical": asdict(event.physical), "durability": event.durability.name,
            "content": payload, "base64": encoded, "content_type": envelope.content_type,
            "attributes": envelope.attributes, "trace_id": envelope.trace_id.hex(),
            "span_id": envelope.span_id.hex()}


def _bound(value, maximum, name):
    if not 1 <= value <= maximum:
        raise ValueError(f"{name} must be between 1 and {maximum}")
    return value


def read_story(client, story, start_hlc=None, end_hlc=None, limit=1000, timeout=10):
    _bound(limit, 10000, "limit")
    end = Hlc(**end_hlc) if end_hlc else Hlc(time.time_ns())
    events = []
    limited = False
    with client.read(story, Hlc(**start_hlc) if start_hlc else None, end, timeout=timeout) as reader:
        for event in reader:
            if len(events) == limit:
                limited = True
                break
            events.append(_event(event))
        completion = asdict(reader.completion) if reader.completion else None
        if completion is not None:
            completion["reason"] = reader.completion.reason.name
        return {"events": events, "completion": completion, "limited": limited,
                "continuation": asdict(reader.continuation) if reader.continuation else None,
                "after": {"hlc": events[-1]["hlc"], "id": events[-1]["id"]} if events else None}


def main():
    parser = argparse.ArgumentParser(description="Read ChronoLog events through Replay")
    parser.add_argument("story", type=int)
    parser.add_argument("--catalog", default=os.getenv("CHRONOLOG_CATALOG", "127.0.0.1:50051"))
    parser.add_argument("--player", default=os.getenv("CHRONOLOG_PLAYER"))
    parser.add_argument("--start-hlc", type=json.loads)
    parser.add_argument("--end-hlc", type=json.loads)
    parser.add_argument("--limit", type=int, default=1000)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    client = connect(args.catalog, args.player, timeout=args.timeout)
    print(_json(read_story(client, args.story, args.start_hlc, args.end_hlc, args.limit, args.timeout)))


if __name__ == "__main__":
    main()
