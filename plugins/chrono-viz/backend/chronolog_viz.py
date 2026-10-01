import asyncio
import base64
from dataclasses import asdict
import json
import os
import time
from types import SimpleNamespace

import chronolog as cl
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel, Field, model_validator

app = FastAPI()
DEADLINE = 5.0


def client():
    return cl.connect(os.getenv("CHRONOLOG_CATALOG", "127.0.0.1:50051"),
                      os.getenv("CHRONOLOG_PLAYER", "127.0.0.1:50054"), timeout=DEADLINE)


@app.exception_handler(cl.Error)
async def sdk_error(request, error):
    code = {3: 400, 5: 404, 4: 504}.get(error.status.code, 503)
    return JSONResponse(status_code=code, content={"detail": str(error)})


def resolve(c, chronicle, story, timeout=DEADLINE):
    for item in c.list_stories(chronicle, timeout=timeout):
        if item.name == story and not item.tombstoned:
            return item
    raise HTTPException(404, "story not found")


@app.get("/health")
def health():
    c = client()
    c.list_chronicles(timeout=2)
    # A missing story still exercises Replay on the configured Player.
    try:
        with c.read(2**63 - 1, cl.Hlc(), cl.Hlc(1), timeout=2) as stream:
            list(stream)
    except cl.NotFound:
        pass
    except cl.FailedPrecondition as error:
        if error.status.message != "unknown story":
            raise
    return {"status": "healthy", "visor": True, "player": True}


@app.get("/stories")
def stories(chronicle: str | None = None):
    c = client()
    if chronicle is None:
        return {"chronicles": [x.name for x in c.list_chronicles(timeout=DEADLINE) if not x.tombstoned]}
    return {"chronicle": chronicle,
            "stories": [x.name for x in c.list_stories(chronicle, timeout=DEADLINE) if not x.tombstoned]}


class Query(BaseModel):
    chronicle: str = Field(min_length=1)
    story: str = Field(min_length=1)
    from_ns: int = Field(ge=0, le=2**63-1)
    to_ns: int = Field(ge=0, le=2**63-1)
    limit: int = Field(default=10000, ge=1, le=10000)
    fields: list[str] = Field(default_factory=list, max_length=128)
    axis: str = "hlc"

    @model_validator(mode="after")
    def range_valid(self):
        if self.to_ns < self.from_ns:
            raise ValueError("to_ns must be >= from_ns")
        if len(set(self.fields)) != len(self.fields) or any(x in {"time", "event_id", "labels"} for x in self.fields):
            raise ValueError("fields must be unique payload names")
        return self


def position(event):
    raw = json.dumps({"hlc": asdict(event.hlc), "id": asdict(event.id)}, separators=(",", ":"))
    return base64.urlsafe_b64encode(raw.encode()).decode()


def decode_position(token):
    try:
        data = json.loads(base64.b64decode(token, altchars=b"-_", validate=True))
        h = cl.Hlc(**data["hlc"])
        eid = cl.EventId(**data["id"])
        if type(h.physical_ns) is not int or type(h.logical) is not int:
            raise ValueError()
        if not (0 <= h.physical_ns < 2**63 and 0 <= h.logical < 2**32):
            raise ValueError()
        if any(type(x) is not int or not 0 < x < 2**64 for x in asdict(eid).values()):
            raise ValueError()
        return SimpleNamespace(hlc=h, id=eid)
    except (ValueError, TypeError, KeyError):
        raise HTTPException(400, "invalid after position")


def row(event, fields):
    try:
        payload = json.loads(event.payload)
    except (ValueError, UnicodeDecodeError):
        payload = {}
    if not isinstance(payload, dict):
        payload = {}
    return [event.hlc.physical_ns / 1_000_000,
            ":".join(str(v) for v in asdict(event.id).values()),
            *[payload.get(name) for name in fields], event.envelope.attributes]


@app.post("/query")
def query(q: Query):
    if q.axis != "hlc":
        raise HTTPException(400, "physical range needs SDK physical read" if q.axis == "physical" else "unknown axis")
    end = time.monotonic() + DEADLINE
    c = client()
    story = resolve(c, q.chronicle, q.story, max(.001, end - time.monotonic()))
    rows = []
    size = 0
    limited = False
    with c.read(story, cl.Hlc(q.from_ns), cl.Hlc(q.to_ns),
                timeout=max(.001, end - time.monotonic())) as stream:
        for event in stream:
            values = row(event, q.fields)
            size += len(json.dumps(values).encode())
            if size > 16 << 20:
                limited = True
                break
            rows.append(values)
            if len(rows) == q.limit:
                limited = True
                break
        completion = stream.completion
    meta = {"complete": None, "reason": None, "frontier": None, "laggards": [], "limited": limited}
    if not limited:
        if completion is None:
            raise HTTPException(502, "Replay ended without Completion")
        meta.update(complete=completion.complete, reason=completion.reason.name,
                    frontier=asdict(completion.frontier),
                    laggards=[asdict(x) for x in completion.laggards])
    columns = [{"name": "time", "type": "time"}, {"name": "event_id", "type": "string"}]
    for i, name in enumerate(q.fields, 2):
        sample = next((r[i] for r in rows if r[i] is not None), None)
        kind = "boolean" if isinstance(sample, bool) else "number" if isinstance(sample, (int, float)) else "string"
        columns.append({"name": name, "type": kind})
        if kind == "string":
            for r in rows:
                if r[i] is not None and not isinstance(r[i], str):
                    r[i] = json.dumps(r[i], separators=(",", ":"))
    columns.append({"name": "labels", "type": "other"})
    return {"columns": columns, "rows": rows, "meta": meta}


def pull(stream):
    return next(stream, None)


@app.get("/tail")
async def tail(request: Request, chronicle: str, story: str, after: str | None = None):
    token = after or request.headers.get("last-event-id")
    previous = decode_position(token) if token else None
    c = client()
    found = await asyncio.to_thread(resolve, c, chronicle, story)
    if previous and previous.id.story_id != found.id:
        raise HTTPException(400, "after belongs to another story")
    stream = await asyncio.to_thread(c.tail, found, previous, timeout=300)

    async def events():
        pending = None
        try:
            while not await request.is_disconnected():
                if pending is None:
                    pending = asyncio.create_task(asyncio.to_thread(pull, stream))
                done, _ = await asyncio.wait({pending}, timeout=.25)
                if not done:
                    yield ": keepalive\n\n"
                    continue
                event = pending.result()
                pending = None
                if event is None:
                    break
                data = {"hlc": asdict(event.hlc), "event_id": {key: str(value) for key, value in asdict(event.id).items()},
                        "payload": event.payload.decode("utf-8", errors="replace"),
                        "labels": event.envelope.attributes}
                yield f"id: {position(event)}\ndata: {json.dumps(data)}\n\n"
        except cl.Error as error:
            yield f"event: error\ndata: {json.dumps({'detail': str(error)})}\n\n"
        finally:
            stream.cancel()
            if pending:
                try:
                    await asyncio.wait_for(asyncio.shield(pending), timeout=2)
                except (asyncio.CancelledError, TimeoutError, cl.Error):
                    pass
    return StreamingResponse(events(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})
