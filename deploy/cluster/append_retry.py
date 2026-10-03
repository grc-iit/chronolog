import time

import grpc


def append(journal, request, seconds=10, grant_deadline=None):
    # Match client/cpp/lib/writer.cpp: preserve unresolved items and back off 20 ms.
    deadline = min(time.monotonic() + seconds, grant_deadline or float('inf'))
    pending = list(range(len(request.items)))
    results = [None] * len(pending)
    last = None
    for attempt in range(501):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        wire = type(request)()
        wire.CopyFrom(request)
        del wire.items[:]
        wire.items.extend(request.items[index] for index in pending)
        try:
            response = journal.Append(wire, timeout=min(seconds, remaining))
            assert response.batch_id == request.batch_id and len(response.results) == len(pending), response
            unresolved = []
            for index, result in zip(pending, response.results):
                if result.status.code == 14:
                    last = str(result)
                    unresolved.append(index)
                else:
                    results[index] = result
            pending = unresolved
            if not pending:
                del response.results[:]
                response.results.extend(results)
                return response
        except grpc.RpcError as error:
            if error.code() != grpc.StatusCode.UNAVAILABLE:
                raise
            last = str(error)
        if attempt < 500:
            time.sleep(min(.02, max(0, deadline - time.monotonic())))
    raise RuntimeError(f'append UNAVAILABLE retry budget exhausted: {last}')
