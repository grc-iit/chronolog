# ChronoLog client library

C++ and Python client for ChronoLog (connect to ChronoVisor, create chronicles/stories, write/read events).

## Error codes

See `client_errcode.h` for the full list. Commonly used:

| Code | Name                  | Meaning |
|------|------------------------|--------|
| 0    | CL_SUCCESS             | Success |
| -4   | CL_ERR_ACQUIRED        | Already acquired, cannot destroy. From **Disconnect()**: the visor could not remove the client record because the client still has acquired stories. |
| -5   | CL_ERR_NOT_ACQUIRED    | Not acquired, cannot release |
| -6   | CL_ERR_CHRONICLE_EXISTS| Chronicle already exists (e.g. CreateChronicle; treat as success if reusing the chronicle is intended) |
| -8   | CL_ERR_NO_CONNECTION   | No connection to ChronoVisor or client is shutting down |

## Recommended shutdown sequence (writer)

1. For each acquired story: call **ReleaseStory**(chronicle_name, story_name).
2. Optionally call **DestroyStory** and **DestroyChronicle** if you are destroying resources.
3. Call **Disconnect()**.
4. Destroy the Client (e.g. `delete client_` or let it go out of scope).

If **Disconnect()** returns **CL_ERR_ACQUIRED** (-4), the visor reports that the client still had acquired stories and did not remove the client record. You may still safely delete the Client: the destructor will perform local teardown and the client is designed to be safe to delete regardless of Disconnect()'s return value. No need to leak the client or skip `delete client_`.

## CreateChronicle and -6 (CL_ERR_CHRONICLE_EXISTS)

If CreateChronicle returns **CL_ERR_CHRONICLE_EXISTS** (-6), a chronicle with that name already exists. It is safe to treat this as success and continue with AcquireStory when reusing an existing chronicle.
