# ChronoLog tail-read example: StoryHandle.playback(n, events)
#
# playback() returns the most recent `n` events of a story directly from the
# keepers' in-memory tail (sealed-but-not-yet-archived chunks), without going
# through the player/archive path used by Client.ReplayStory().
#
# Two things to know about playback():
#   1. It is a writer-mode API: a Client constructed with only the portal
#      configuration (no query service configuration) can use it.
#   2. Events become visible in the tail once their story chunk SEALS, i.e.
#      after chunk_duration + acceptance_window (keeper configuration; ~25-30s
#      with the default local deployment). Events still in the active/open
#      chunk are not returned.
#
# Run with no arguments for the original single-process demo: it writes events,
# waits for them to seal, then plays them back.
#
#     python3 tail_reader_client.py
#
# The writer and the reader can also run as separate processes, which is how the
# CI pipeline exercises this path across two containers:
#
#     # container A -- write, then hold the story so the chunks stay in the tail
#     python3 tail_reader_client.py --config <client.json> --mode write --hold 150
#
#     # container B -- acquire the same story and read it back
#     python3 tail_reader_client.py --config <client.json> --mode read --expect-min 1
#
# The writer HOLDS the story rather than releasing it: releasing retires the
# story pipeline, and a retired pipeline hands its chunks straight to the
# extraction queue instead of the tail, so playback() would find nothing.
#
# See writer_client.py for the post-installation steps (PYTHONPATH /
# LD_LIBRARY_PATH / py_chronolog_client.so symlink) needed to import the
# py_chronolog_client module.

import argparse
import json
import sys
import time


def parse_args():
    parser = argparse.ArgumentParser(description="ChronoLog tail-read (playback) example")
    parser.add_argument(
        "--config",
        help="ChronoLog client config JSON; its VisorClientPortalService block "
        "supplies the connection details instead of the --visor-* defaults",
    )
    parser.add_argument("--visor-ip", default="127.0.0.1")
    parser.add_argument("--visor-port", type=int, default=5555)
    parser.add_argument("--visor-provider", type=int, default=55)
    parser.add_argument("--protocol", default="ofi+sockets")
    parser.add_argument("--chronicle", default="py_chronicle")
    parser.add_argument("--story", default="tail_story")
    parser.add_argument(
        "--mode",
        choices=["both", "write", "read"],
        default="both",
        help="both: write, settle and read in one process (default). "
        "write/read: split across two processes or hosts.",
    )
    parser.add_argument("--events", type=int, default=30, help="events to log in write/both mode")
    parser.add_argument(
        "--settle",
        type=int,
        default=40,
        help="both mode: seconds to wait after writing so the chunks seal",
    )
    parser.add_argument(
        "--hold",
        type=int,
        default=0,
        help="write mode: seconds to keep the story acquired after writing, so a "
        "separate reader can play the events back before the pipeline retires",
    )
    parser.add_argument("--playback-n", type=int, default=10)
    parser.add_argument(
        "--expect-min",
        type=int,
        default=0,
        help="read mode: exit non-zero if playback returns fewer events than this",
    )
    parser.add_argument(
        "--read-timeout",
        type=int,
        default=0,
        help="read mode: keep retrying playback for up to this many seconds until "
        "--expect-min events come back. A single shot is inherently racy: events "
        "are only in the tail between their chunk sealing (chunk_duration + "
        "acceptance_window) and ageing out (tail_retention_secs measured from the "
        "chunk's end), so the readable window is roughly "
        "tail_retention_secs - acceptance_window_secs.",
    )
    return parser.parse_args()


def portal_conf_from(args, py_chronolog_client):
    """Build a ClientPortalServiceConf from --config when given, else the flags."""
    protocol, ip, port, provider = (
        args.protocol,
        args.visor_ip,
        args.visor_port,
        args.visor_provider,
    )
    if args.config:
        with open(args.config) as conf_file:
            rpc = json.load(conf_file)["chrono_client"]["VisorClientPortalService"]["rpc"]
        protocol = rpc.get("protocol_conf", protocol)
        ip = rpc.get("service_ip", ip)
        port = int(rpc.get("service_base_port", port))
        provider = int(rpc.get("service_provider_id", provider))
    print(" connecting to ChronoVisor at {}://{}:{}@{}".format(protocol, ip, port, provider))
    return py_chronolog_client.ClientPortalServiceConf(protocol, ip, port, provider)


def tail_reader_client():
    args = parse_args()
    print("Basic test for py_chronolog_client tail read (playback), mode =", args.mode)

    import py_chronolog_client

    # writer mode: portal conf only, no query service configuration needed
    client = py_chronolog_client.Client(portal_conf_from(args, py_chronolog_client))

    # Connect to the ChronoVisor for client authentication
    # returns 0 on success and error_code otherwise
    return_code = client.Connect()
    print("\n client.Connect() call returns:", return_code)
    if return_code != 0:
        return 1

    # create chronicle (already-exists is not an error for our purposes)
    return_code = client.CreateChronicle(args.chronicle)
    print("\n client.CreateChronicle() returned", return_code)

    # acquire the story; both the writer and the reader acquire the SAME story
    # returns a tuple (0, StoryHandle) on success and (error_code, None) otherwise
    return_tuple = client.AcquireStory(args.chronicle, args.story)
    print("\n client.AcquireStory() returned:", return_tuple)
    if return_tuple[0] != 0:
        client.Disconnect()
        return 1
    story_handle = return_tuple[1]

    exit_code = 0

    if args.mode in ("both", "write"):
        print("\n logging", args.events, "events for", args.story, "using the StoryHandle")
        for i in range(args.events):
            story_handle.log_event("py_tail_event.{}".format(i))

    if args.mode == "both":
        # keep the story acquired while the chunks seal into the keeper tail
        # (chunk_duration + acceptance_window; ~30s for the default local deploy)
        print("\n holding the story acquired for", args.settle, "seconds so the events seal...")
        time.sleep(args.settle)

    if args.mode == "write" and args.hold > 0:
        # hold the story so a reader elsewhere can play these events back: a
        # released story retires, and a retired pipeline bypasses the tail
        print("\n holding the story acquired for", args.hold, "seconds for a separate reader...")
        time.sleep(args.hold)

    if args.mode in ("both", "read"):
        # playback fills an EventList in place and returns 0 on success
        deadline = time.time() + args.read_timeout
        while True:
            events = py_chronolog_client.EventList()
            return_code = story_handle.playback(args.playback_n, events)
            if len(events) >= args.expect_min or time.time() >= deadline:
                break
            time.sleep(5)
        print("\n story_handle.playback({}, events) returned:".format(args.playback_n), return_code)
        print(" playback returned", len(events), "event(s):")
        for i, event in enumerate(events):
            print("    [{}] time={} : {}".format(i, event.time(), event.log_record()))

        # asking for more than is available returns everything in the tail
        all_events = py_chronolog_client.EventList()
        return_code = story_handle.playback(1000, all_events)
        print(
            "\n story_handle.playback(1000, events) returned:",
            return_code,
            "with",
            len(all_events),
            "event(s) (the whole tail)",
        )

        if len(all_events) < args.expect_min:
            print(
                "\n FAIL: expected at least {} event(s) from the keeper tail, got {}".format(
                    args.expect_min, len(all_events)
                )
            )
            exit_code = 1

    # release acquired Story
    return_code = client.ReleaseStory(args.chronicle, args.story)
    print("\n client.ReleaseStory() returned:", return_code)

    # Disconnect the client from the ChronoLog system
    return_code = client.Disconnect()
    print("\n client.Disconnect() returned:", return_code)
    return exit_code


if __name__ == "__main__":
    sys.exit(tail_reader_client())
