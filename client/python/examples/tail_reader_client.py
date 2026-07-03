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
#      with the default local deployment). The writer below therefore holds
#      the story acquired while it waits before playing back. Events still in
#      the active/open chunk are not returned.
#
# See writer_client.py for the post-installation steps (PYTHONPATH /
# LD_LIBRARY_PATH / py_chronolog_client.so symlink) needed to import the
# py_chronolog_client module.

import time


def tail_reader_client():
    print("Basic test for py_chronolog_client tail read (playback)")

    import py_chronolog_client

    # create ClientPortalServiceConf instance with the connection credentials
    # to ChronoVisor ClientPortalService (writer mode: portal conf only)
    clientConf = py_chronolog_client.ClientPortalServiceConf(
        "ofi+sockets", "127.0.0.1", 5555, 55
    )

    # instantiate ChronoLog Client object
    client = py_chronolog_client.Client(clientConf)

    # Connect to the ChronoVisor for client authentication
    # returns 0 on success and error_code otherwise
    return_code = client.Connect()
    print("\n client.Connect() call returns:", return_code)

    # create chronicle
    return_code = client.CreateChronicle("py_chronicle")
    print("\n client.CreateChronicle() returned", return_code)

    # acquire story that is part of "py_chronicle"
    # returns a tuple (0, StoryHandle) on success and (error_code, None) otherwise
    return_tuple = client.AcquireStory("py_chronicle", "tail_story")
    print("\n client.AcquireStory() returned:", return_tuple)

    if return_tuple[0] != 0:
        client.Disconnect()
        return

    story_handle = return_tuple[1]

    # log some events
    event_count = 30
    print("\n logging", event_count, "events for tail_story using the StoryHandle")
    for i in range(event_count):
        story_handle.log_event("py_tail_event.{}".format(i))

    # keep the story acquired while the chunks seal into the keeper tail
    # (chunk_duration + acceptance_window; ~30s for the default local deploy)
    settle_seconds = 40
    print(
        "\n holding the story acquired for",
        settle_seconds,
        "seconds so the events seal into the keeper tail...",
    )
    time.sleep(settle_seconds)

    # play back the most recent 10 events (expects events #20..#29, ascending);
    # playback fills an EventList in place and returns 0 on success
    events = py_chronolog_client.EventList()
    return_code = story_handle.playback(10, events)
    print("\n story_handle.playback(10, events) returned:", return_code)
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

    # release acquired Story
    return_code = client.ReleaseStory("py_chronicle", "tail_story")
    print("\n client.ReleaseStory() returned:", return_code)

    # Disconnect the client from the ChronoLog system
    return_code = client.Disconnect()
    print("\n client.Disconnect() returned:", return_code)


if __name__ == "__main__":
    tail_reader_client()
