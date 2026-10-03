# Drops a live ContextClient while a follow is still open. "drain" joins the follow and collects the session before
# exit; "exit" leaves a daemon thread inside a native follow and a suspended subscription when the interpreter exits.
import gc
import os
import sys
import threading
import time
import weakref

import chronolog as cl

options = cl.ContextOptions(os.environ["CHRONOLOG_TEST_VISOR"], os.environ.get("CHRONOLOG_TEST_PLAYER"), timeout=8)
mode = sys.argv[1]
watched = []
kept = []


def watch(obj, label):
    watched.append(weakref.ref(obj, lambda _: print(f"collected {label}", flush=True)))


def idle():
    # Its wrappers become garbage at once: the reaper drops a live Writer off the interpreter thread.
    contexts = cl.connect_context(options)
    ref = contexts.ensure_context(f"python-finalizer-{os.getpid()}-{time.time_ns()}", "idle")
    watch(contexts.open(ref, cl.AgentIdentity("finalizer", "idle")), "idle session")


def abandon():
    contexts = cl.connect_context(options)
    ref = contexts.ensure_context(f"python-finalizer-{os.getpid()}-{time.time_ns()}", mode)
    session = contexts.open(ref, cl.AgentIdentity("finalizer", mode))
    result = session.remember(cl.Memory(mode, cl.Envelope(mode.encode())))
    if result.current.outcome is not cl.MemoryOutcome.DURABLE:
        raise RuntimeError(f"remember {result.current.outcome!r}")
    after = cl.Position(result.current.receipt.hlc, result.current.receipt.event_id)
    subscription = contexts.subscribe([cl.FollowInput(session, cl.FollowFrom.BEGINNING)],
                                      options=cl.FollowOptions(wait=1))
    next(subscription)
    started = threading.Event()

    def follow(inputs):
        started.set()
        contexts.follow(inputs, options=cl.FollowOptions(wait=3))

    thread = threading.Thread(target=follow, args=([cl.FollowInput(session, cl.FollowFrom.POSITION, after)],),
                              daemon=mode == "exit")
    watch(session, f"{mode} session")
    thread.start()
    started.wait()
    return thread, subscription


idle()
thread, subscription = abandon()
if mode == "drain":
    thread.join()
    del thread, subscription
    gc.collect()
else:
    kept.append(subscription)
