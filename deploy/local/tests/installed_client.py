import sys
from pathlib import Path

import chronolog as cl


def main():
    prefix, catalog, player = sys.argv[1:]
    assert Path(cl.__file__).resolve().is_relative_to(Path(prefix).resolve()), 'FAIL binding must be installed in prefix'
    client = cl.connect(catalog, player, timeout=5, max_retries=0)
    chronicle = client.create_chronicle('installed-client', timeout=5)
    story = client.create_story(chronicle, 'events', timeout=5)
    with client.acquire(story, 'installed-writer', timeout=5) as writer:
        result = writer.append(b'installed durable event', durability=cl.Durability.DURABLE, timeout=5)
        assert result.acked and result.durability == cl.Durability.DURABLE, 'FAIL DURABLE append'
        end = cl.Hlc(result.hlc.physical_ns, result.hlc.logical + 1)
        with client.read(story, result.hlc, end, timeout=5) as reader:
            events = list(reader)
            assert reader.completion is not None and reader.completion.complete, 'FAIL complete read'
        assert len(events) == 1, 'FAIL event count'
        assert events[0].id == result.event_id and events[0].hlc == result.hlc, 'FAIL identity or HLC'
        assert events[0].envelope.payload == b'installed durable event', 'FAIL payload'
    print('PASS installed binding: DURABLE append and complete read')


if __name__ == '__main__':
    main()
