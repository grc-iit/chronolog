#ifndef CHRONOLOG_LDMS_BRIDGE_H
#define CHRONOLOG_LDMS_BRIDGE_H
/* C99 bridge between ldmsd store plugins and the ChronoLog C++ client SDK.
 * store() never blocks: samples enter a bounded queue and a drainer thread appends them in batches. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        CHRONO_LDMS_OK = 0,
        CHRONO_LDMS_INVALID_ARGUMENT = 1,
        CHRONO_LDMS_UNAVAILABLE = 2,
        CHRONO_LDMS_TIMEOUT = 3,
        CHRONO_LDMS_DROPPED = 4,
        CHRONO_LDMS_CLOSED = 5
    };

    enum
    {
        CHRONO_LDMS_DURABLE = 0,
        CHRONO_LDMS_ACCEPTED = 1
    };

    typedef struct chrono_ldms chrono_ldms_t;
    typedef struct chrono_ldms_story chrono_ldms_stream_t;

    /* Called from the drainer thread only, at most once per five seconds. */
    typedef void (*chrono_ldms_log_fn)(void* ctx, const char* message);

    /* Zero means default for every numeric field: capacity 8192, batch 128, flush 100 ms, rpc 10000 ms, durable. */
    typedef struct
    {
        const char* catalog_endpoint;
        const char* player_endpoint;
        uint32_t queue_capacity;
        uint32_t batch_size;
        uint32_t flush_interval_ms;
        uint32_t rpc_timeout_ms;
        int durability;
        chrono_ldms_log_fn log;
        void* log_ctx;
    } chrono_ldms_config;

    typedef struct
    {
        uint64_t queued;
        uint64_t appended;
        uint64_t dropped;
        uint64_t failed;
        char last_error[256];
    } chrono_ldms_stats;

    enum
    {
        CHRONO_LDMS_I64 = 0,
        CHRONO_LDMS_U64 = 1,
        CHRONO_LDMS_F64 = 2
    };

    typedef struct
    {
        const char* name;
        int kind;
        union
        {
            int64_t i64;
            uint64_t u64;
            double f64;
        } v;
    } chrono_ldms_metric;

    typedef struct
    {
        const char* producer;
        const char* instance;
        const char* schema;
        uint64_t component_id;
        uint64_t timestamp_ns;
        const chrono_ldms_metric* metrics;
        size_t metric_count;
    } chrono_ldms_sample;

    /* Connects to the Catalog (and Player when given). On failure *out is NULL and error holds the reason. */
    int chrono_ldms_open(const chrono_ldms_config* config, chrono_ldms_t** out, char* error, size_t error_cap);

    /* Maps container to a chronicle and schema plus producer to a story. Idempotent and cheap; the Catalog is
 * contacted lazily by the drainer, so this never performs I/O. The stream lives until close. */
    int chrono_ldms_stream(chrono_ldms_t* bridge,
                           const char* container,
                           const char* schema,
                           const char* producer,
                           chrono_ldms_stream_t** out);

    /* Queues one JSON sample object. Returns CHRONO_LDMS_DROPPED when the queue is full. Never blocks. */
    int chrono_ldms_store(chrono_ldms_stream_t* stream, uint64_t timestamp_ns, const char* json, size_t len);

    /* Writes the JSON sample document and returns its length, which may exceed cap (then out is truncated). Returns
 * a negative value for an invalid sample. */
    int64_t chrono_ldms_encode(const chrono_ldms_sample* sample, char* out, size_t cap);

    /* Encodes the sample, resolves its stream and queues it. */
    int chrono_ldms_store_sample(chrono_ldms_t* bridge, const char* container, const chrono_ldms_sample* sample);

    /* Waits until everything queued before the call was appended or failed. */
    int chrono_ldms_flush(chrono_ldms_t* bridge, uint32_t timeout_ms);

    void chrono_ldms_stats_get(chrono_ldms_t* bridge, chrono_ldms_stats* out);

    /* Drains for up to drain_timeout_ms, counts the remainder as failed and frees the bridge. */
    int chrono_ldms_close(chrono_ldms_t* bridge, uint32_t drain_timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
