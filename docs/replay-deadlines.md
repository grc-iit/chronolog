# Replay deadlines and retained queries

`Client::ReplayStory` uses one steady-clock budget from API entry. The default
is 180000 ms. Policy admission, the reader map snapshot, outbound queueing,
provider registration, address lookup, submission and result waiting consume
that same budget. One submission is allowed; its control expiry is the earlier
of the original replay expiry and control phase start plus `attempt_ms` (5000 ms
by default). A native timeout does not cancel the remote query or prove that
native state can be destroyed.

The public caller coordinates the query without occupying an RPC worker. One
job in the existing two-worker client runtime owns its route, arguments and
native callable. A matching private response can complete the public operation
while the scalar submission ACK is still pending. An unknown ACK is not
retried. Unknown native work retains its worker slot and query ownership,
possibly until process exit.

The actual client context permits two logical query records, independently of
its two control slots and two incoming bulk slots. Public waiters, queued or
running submission jobs, incoming responses and quarantined work share those
query permits. Capacity exhaustion returns `CL_ERR_RPC_CAPACITY`.

Vector output stays private until the live public caller consumes it. Late
work has no caller vector reference. Only the handler that claims publication
owns staging consumption or discard. A fully pulled duplicate gets receipt 0
and releases its incoming permit without touching the publisher's staging. Streaming callbacks run on the native
receive thread. Each callback exception is caught and logged; that event is
skipped and later events continue while admission remains open. Expiry closes
callback admission. A callback already running must return cooperatively, and
the public call waits for that callback and releases its captured callable
before returning. Its execution time is the stated exception to library wait
budgets. No new full library budget starts after it finishes.

The unchanged wire uses a uint32 query identifier and has no incarnation field.
IDs 1 through 2147483647 avoid the negative error-scalar range. Their allocator
has process lifetime while the library stays loaded, including reconnects and
native context replacement. IDs are never reused; exhaustion refuses admission.
A process restart using the same response endpoint cannot distinguish every
stale response from the prior process with this wire format. Keep the library
loaded while retained work exists.

Map cleanup never spends an extra wait budget after expiry. If its try-lock
fails, at most two consumed records stay charged until the next replay prunes
them before new admission. `ClientQueryService::pruneConsumedQueries()` also
provides a nonwaiting lifecycle hook. B12 must prune those inert roots before
using native-use/query counts as idle evidence, close incoming admission, and
prove native settlement. The hook alone proves none of those other conditions;
reader teardown remains incomplete until B12. Large staged vectors are consumed
or discarded by the B10 handoff; discard frees their capacity on the receive
ULT.
