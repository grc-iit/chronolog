---
sidebar_position: 3
title: "Error Codes"
---

# Error Codes

All ChronoLog client API calls return an `int`. `0` means success; negative values indicate an error. The error codes are split into two ranges: client-side codes (`0` to `-12`, defined in `client_errcode.h`) and server-side codes (`-101` to `-112`, defined in `chronolog_errcode.h`).

## Client Error Codes

Returned by the client library when the local client or the connection to ChronoVisor is the source of the problem.

| Code | Value | Meaning | Recommended action |
|---|---|---|---|
| `CL_SUCCESS` | 0 | Operation succeeded. | — |
| `CL_ERR_UNKNOWN` | -1 | Unexpected or unclassified error. | Check component logs for details. |
| `CL_ERR_INVALID_ARG` | -2 | An argument passed to the API call is invalid (null pointer, empty name, out-of-range value, …). | Verify argument types and values. |
| `CL_ERR_NOT_EXIST` | -3 | The referenced Chronicle, Story, or property does not exist. | Create the resource before using it. |
| `CL_ERR_ACQUIRED` | -4 | The resource is still acquired and cannot be destroyed. | Release all acquisitions first. |
| `CL_ERR_NOT_ACQUIRED` | -5 | The Story has not been acquired by this client; cannot log events or release. | Call `AcquireStory` before logging or releasing. |
| `CL_ERR_CHRONICLE_EXISTS` | -6 | A Chronicle with that name already exists; cannot create a duplicate. | Use the existing Chronicle, or choose a different name. |
| `CL_ERR_NO_KEEPERS` | -7 | ChronoVisor has no registered ChronoKeeper nodes available. | Verify at least one `chrono-keeper` process is running and has registered. |
| `CL_ERR_NO_CONNECTION` | -8 | The client has no connection to ChronoVisor. | Check Visor address, port, and protocol in the client config. |
| `CL_ERR_NOT_AUTHORIZED` | -9 | The operation is not permitted under the current RBAC policy. | Check the authentication/RBAC configuration. |
| `CL_ERR_NO_PLAYERS` | -10 | No ChronoPlayer nodes are available to serve replay queries. | Verify at least one `chrono-player` process is running. |
| `CL_ERR_NOT_READER_MODE` | -11 | The client was opened in `WRITER_MODE`; replay queries require a reader-mode client. | Open a separate client instance in reader mode for queries. |
| `CL_ERR_QUERY_TIMED_OUT` | -12 | A replay query did not complete within the allowed timeout. | Retry the query, or investigate Player load and storage latency. |

## Server Error Codes

Returned by the server-side components (ChronoVisor, ChronoKeeper, ChronoGrapher, ChronoPlayer) and propagated back through the client library.

| Code | Value | Meaning | Recommended action |
|---|---|---|---|
| `CL_ERR_STORY_EXISTS` | -101 | A Story with that name already exists in the Chronicle. | Use the existing Story, or choose a different name. |
| `CL_ERR_ARCHIVE_EXISTS` | -102 | An archive with that identifier already exists. | Use the existing archive, or choose a different name. |
| `CL_ERR_CHRONICLE_PROPERTY_FULL` | -103 | The Chronicle's property list has reached its capacity. | Remove unused Chronicle properties before adding new ones. |
| `CL_ERR_STORY_PROPERTY_FULL` | -104 | The Story's property list has reached its capacity. | Remove unused Story properties before adding new ones. |
| `CL_ERR_CHRONICLE_METADATA_FULL` | -105 | The Chronicle's metadata list has reached its capacity. | Remove unused Chronicle metadata entries before adding new ones. |
| `CL_ERR_STORY_METADATA_FULL` | -106 | The Story's metadata list has reached its capacity. | Remove unused Story metadata entries before adding new ones. |
| `CL_ERR_INVALID_CONF` | -107 | A component received an invalid or incomplete configuration. | Validate the JSON config file against the schema; check for missing required fields. |
| `CL_ERR_STORY_CHUNK_EXISTS` | -108 | A story chunk with that ID already exists (internal duplicate). | This indicates unexpected internal state; restart the affected ChronoKeeper. |
| `CL_ERR_CHRONICLE_DIR_NOT_EXIST` | -109 | The storage directory for the Chronicle does not exist on the Grapher/Player host. | Verify `story_files_dir` in the Grapher and Player config points to an existing, writable path. |
| `CL_ERR_STORY_FILE_NOT_EXIST` | -110 | The expected Story file is missing on disk. | Check disk availability and the `story_files_dir` path; the file may have been deleted externally. |
| `CL_ERR_STORY_CHUNK_DSET_NOT_EXIST` | -111 | A story chunk dataset was not found in the archive during replay. | Check Grapher storage integrity and logs. |
| `CL_ERR_STORY_CHUNK_EXTRACTION` | -112 | An error occurred while extracting a story chunk inside ChronoKeeper. | Inspect ChronoKeeper logs (`chrono-keeper-<N>.log`) for the root cause. |
