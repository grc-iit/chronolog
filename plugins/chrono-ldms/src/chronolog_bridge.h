/*
 * chronolog_bridge.h -- C ABI bridging the (C) ldmsd store plugin glue in
 * store_chronolog.c to the (C++) ChronoLog client implemented in
 * chronolog_bridge.cpp.
 *
 * ldmsd.h is not C++-safe, so the ldmsd plugin interface must live in a C
 * translation unit; the ChronoLog client only offers a C++ API, so it must
 * live in a C++ translation unit. This header is the seam between them. Only
 * the C++-safe ldms.h is shared (needed for ldms_set_t and the metric
 * accessors used during serialization).
 */
#ifndef STORE_CHRONOLOG_BRIDGE_H
#define STORE_CHRONOLOG_BRIDGE_H

#include <stddef.h>

/* ldms.h must be included with C linkage from the C++ bridge TU. The header
 * comment above asserts ldms.h is "C++-safe", but nothing in this repository
 * verifies that: the plugin links no LDMS library (the ldms_* symbols are
 * resolved by ldmsd at dlopen time), so a mangling mismatch would not surface at
 * build time -- it would surface as `undefined symbol: _Z18ldms_metric_get_u64...`
 * when ldmsd loads libstore_chronolog.so. Wrapping the include is a no-op if
 * ldms.h already guards itself, and is what makes the assertion true if it does
 * not. */
#ifdef __cplusplus
extern "C"
{
#endif

#include "ldms.h"

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /* Opaque per-storage-policy handle returned by clog_open(). One strgp covers
     * every producer feeding its container/schema; the ChronoLog story is keyed
     * per producer, so this handle fans out to one story per producer, acquired
     * lazily on that producer's first sample. */
    typedef void* clog_store_t;

    /*
 * Create (if needed) and connect the shared ChronoLog client. Pass the path to
 * a ChronoLog client config JSON, or NULL/"" to use client defaults.
 *
 * A NULL/"" path never overrides an endpoint an earlier call established -- it
 * is the open_store() fallback for "reached without config". A non-empty path
 * that names a DIFFERENT endpoint than the one this process is already bound to
 * fails with EINVAL rather than being ignored: the ChronoLog client is a
 * process-wide singleton whose endpoint is fixed at first construction, so the
 * request genuinely cannot be honoured and silently keeping the old address
 * would point the plugin at the wrong ChronoVisor.
 *
 * Otherwise idempotent: after a successful connect, matching calls are no-ops,
 * and a call after a failed connect retries it.
 *
 * Returns 0 on success, errno-style code on failure.
 */
    int clog_connect(const char* client_conf_path);

    /*
 * Ensure the chronicle exists and acquire the story. Returns an opaque handle
 * to be passed to clog_store()/clog_close(), or NULL on failure.
 */
    clog_store_t clog_open(const char* chronicle, const char* schema);

    /*
 * Serialize the selected metrics of `set` to a JSON line and log it as one
 * ChronoLog event on the story. Returns 0 on success, errno-style on failure.
 */
    int clog_store(clog_store_t story, ldms_set_t set, int* metric_arry, size_t metric_count);

    /* Release the story and free the handle. */
    void clog_close(clog_store_t story);

    /*
 * Register/unregister one ldmsd plugin instance. The newer OVIS plugin API is
 * per-instance (each `load name=... plugin=store_chronolog` gets its own
 * handle), but the ChronoLog client, its connection and every acquired story
 * are process-wide. Call attach from the plugin constructor and detach from the
 * destructor: the connection is dropped only when the LAST instance detaches,
 * so terminating one instance cannot pull the client out from under another.
 *
 * detach never destroys the client object itself -- see the note in
 * clog_plugin_detach() -- so a plugin reload reconnects rather than reusing
 * freed memory.
 */
    void clog_plugin_attach(void);
    void clog_plugin_detach(void);

    /*
 * Human-readable description of the most recent failure on the calling thread.
 * Valid until the next bridge call on the same thread.
 */
    const char* clog_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STORE_CHRONOLOG_BRIDGE_H */
