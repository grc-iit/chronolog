# Example: exercise the Python bindings for the v3.0.0 metadata surface
# (CreateChronicle / DestroyChronicle, AcquireStory / ReleaseStory /
# DestroyStory, ShowChronicles, ShowStories). Run the same way as
# writer_client.py / reader_client.py.


def metadata_client():
    print("Metadata-surface demo for py_chronolog_client")

    # NOTE: if you having issues importing py_chronolog_client
    # see post installation instructions at the top of writer_client.py

    import py_chronolog_client

    clientConf = py_chronolog_client.ClientPortalServiceConf(
        "ofi+sockets", "127.0.0.1", 5555, 55
    )
    client = py_chronolog_client.Client(clientConf)

    rc = client.Connect()
    print("\n client.Connect() returned:", rc)

    for chronicle in ("py_chronicle_a", "py_chronicle_b"):
        rc = client.CreateChronicle(chronicle)
        print("\n client.CreateChronicle({!r}) returned:".format(chronicle), rc)

    for chronicle, story in (
        ("py_chronicle_a", "story_one"),
        ("py_chronicle_a", "story_two"),
        ("py_chronicle_b", "story_solo"),
    ):
        rc, handle = client.AcquireStory(chronicle, story)
        print(
            "\n client.AcquireStory({!r}, {!r}) returned:".format(chronicle, story),
            (rc, handle),
        )

    show_rc, chronicles = client.ShowChronicles()
    print("\n client.ShowChronicles() returned:", (show_rc, chronicles))

    for chronicle in ("py_chronicle_a", "py_chronicle_b"):
        show_rc, stories = client.ShowStories(chronicle)
        print(
            "\n client.ShowStories({!r}) returned:".format(chronicle),
            (show_rc, stories),
        )

    # Disconnect auto-releases acquired stories; explicitly destroying the
    # chronicles cascades through the stories under each.
    for chronicle in ("py_chronicle_a", "py_chronicle_b"):
        rc = client.DestroyChronicle(chronicle)
        print("\n client.DestroyChronicle({!r}) returned:".format(chronicle), rc)

    rc = client.Disconnect()
    print("\n client.Disconnect() returned:", rc)


if __name__ == "__main__":
    metadata_client()
