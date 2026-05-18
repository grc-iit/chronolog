---
sidebar_position: 3
title: "Error Codes"
---

# Python Client Error Codes

All `Client` methods return a plain `int` status code. A return value of `0` means success — any negative value indicates an error. The Python binding returns the same integer values as the underlying C++ `ClientErrorCode` enum.

## Error Code Table

| Code name | Value | Description |
|-----------|------:|-------------|
| `CL_SUCCESS` | 0 | Operation completed successfully |
| `CL_ERR_UNKNOWN` | -1 | Generic/unexpected error |
| `CL_ERR_INVALID_ARG` | -2 | Invalid argument or parameter |
| `CL_ERR_NOT_EXIST` | -3 | Chronicle or Story does not exist |
| `CL_ERR_ACQUIRED` | -4 | Resource is acquired and cannot be destroyed |
| `CL_ERR_NOT_ACQUIRED` | -5 | Resource is not acquired and cannot be released |
| `CL_ERR_CHRONICLE_EXISTS` | -6 | Chronicle with that name already exists |
| `CL_ERR_NO_KEEPERS` | -7 | No ChronoKeeper nodes available |
| `CL_ERR_NO_CONNECTION` | -8 | No connection to ChronoVisor |
| `CL_ERR_NOT_AUTHORIZED` | -9 | Operation not authorized |
| `CL_ERR_NO_PLAYERS` | -10 | No ChronoPlayer nodes available |
| `CL_ERR_NOT_READER_MODE` | -11 | Client is in WRITER_MODE; read operations unavailable |
| `CL_ERR_QUERY_TIMED_OUT` | -12 | Replay query timed out |
| `CL_ERR_PROTOCOL_VERSION_MISMATCH` | -13 | Client and server disagree on the wire-protocol version |

## Error Handling

Since the Python binding returns raw integers, check return codes with simple comparisons or a lookup table:

```python
import py_chronolog_client

ERROR_NAMES = {
    0:   "CL_SUCCESS",
    -1:  "CL_ERR_UNKNOWN",
    -2:  "CL_ERR_INVALID_ARG",
    -3:  "CL_ERR_NOT_EXIST",
    -4:  "CL_ERR_ACQUIRED",
    -5:  "CL_ERR_NOT_ACQUIRED",
    -6:  "CL_ERR_CHRONICLE_EXISTS",
    -7:  "CL_ERR_NO_KEEPERS",
    -8:  "CL_ERR_NO_CONNECTION",
    -9:  "CL_ERR_NOT_AUTHORIZED",
    -10: "CL_ERR_NO_PLAYERS",
    -11: "CL_ERR_NOT_READER_MODE",
    -12: "CL_ERR_QUERY_TIMED_OUT",
    -13: "CL_ERR_PROTOCOL_VERSION_MISMATCH",
}

portal_conf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets", "127.0.0.1", 5555, 55)
client = py_chronolog_client.Client(portal_conf)

ret = client.Connect()
if ret != 0:
    print(f"Connect failed: {ERROR_NAMES.get(ret, ret)}")
else:
    print("Connected successfully")

ret = client.CreateChronicle("MyChronicle", {}, 1)
if ret == 0 or ret == -6:   # success or already exists — both are acceptable
    print("Chronicle ready")
else:
    print(f"CreateChronicle failed: {ERROR_NAMES.get(ret, ret)}")
```
