---
sidebar_position: 3
title: "Error Codes"
---

# Error Codes

All Client API methods return an `int` status code. Check the return value against `chronolog::CL_SUCCESS` (0) before proceeding — any negative value indicates an error. All codes are defined in the `chronolog` namespace.

```cpp
#include <client_errcode.h>
```

## Client Error Codes

| Code | Value | Description |
|------|------:|-------------|
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

Use `to_string_client(int)` to get the error name as a string for logging:

```cpp
int ret = client.Connect();
if (ret != chronolog::CL_SUCCESS) {
    std::cerr << "Connect failed: " << chronolog::to_string_client(ret) << "\n";
    return 1;
}
```
