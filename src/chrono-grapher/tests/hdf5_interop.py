import subprocess
import sys
import tempfile

try:
    import h5py
except ImportError:
    print("h5py unavailable")
    sys.exit(77)

with tempfile.TemporaryDirectory(prefix="chronolog-hdf5-") as root:
    path = subprocess.check_output([sys.argv[1], root], text=True, timeout=20).strip()
    with h5py.File(path, "r") as archive:
        assert list(archive) == ["chunk"]
        chunk = archive["chunk"]
        assert dict(chunk.attrs) == dict(story_id=42, start_time=100, start_logical=3,
                                        end_time=200, end_logical=5, attribute_backslash_encoding=1)
        events = chunk["events.vlen_bytes"][:]
        assert len(events) == 2
        first = events[0]
        for field, expected in dict(story_id=42, writer_id=7, incarnation=8, sequence=9,
                                    hlc_physical_ns=123, hlc_logical=4, physical_ns=-17,
                                    uncertainty_ns=99, has_uncertainty=1, clock_status=0,
                                    durability=2).items():
            assert first[field] == expected, field
        assert bytes(first["content_type"]) == b"application/octet-stream"
        assert bytes(first["payload"]) == b"a\x00\xffz"
        assert bytes(first["trace_id"]) == bytes(16)
        assert bytes(first["span_id"]) == b"\xff" * 8
        assert bytes(events[1]["payload"]) == b""
        assert events[1]["has_uncertainty"] == 0
        attrs = chunk["attributes"][:]
        assert len(attrs) == 1
        assert tuple(attrs[0]) == (0, b"host", b"dragon")
print("h5py read all event fields, binary and empty payloads, metadata, and attributes")
