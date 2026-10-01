/* LDMS 4.x store plugin: container -> chronicle, schema + producer -> story, one sample -> one event.
 * Compiled only when CHRONOLOG_LDMS_ROOT points at an LDMS install. All ChronoLog work lives in the bridge. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"

#include "chronolog_ldms_bridge.h"

#define PNAME "store_chronolog"

struct store_handle
{
    char* container;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static chrono_ldms_t* bridge;
static unsigned instances;

static void bridge_log(void* ctx, const char* message)
{
    ovis_log((ovis_log_t)ctx, OVIS_LWARN, PNAME ": %s\n", message);
}

static const char* usage(ldmsd_plug_handle_t handle)
{
    (void)handle;
    return "    config name=<inst> catalog=<host:port> [player=<host:port>] [queue=<n>] [batch=<n>]\n"
           "           [flush_ms=<n>] [durability=durable|accepted]\n"
           "        Store samples in ChronoLog. container is the chronicle and <schema>_<producer> the story.\n"
           "        store never blocks: a full queue drops the sample and counts it.\n";
}

static uint32_t uint_attr(struct attr_value_list* avl, const char* key)
{
    char* value = av_value(avl, key);
    return value ? (uint32_t)strtoul(value, NULL, 10) : 0;
}

static int config(ldmsd_plug_handle_t handle, struct attr_value_list* kwl, struct attr_value_list* avl)
{
    ovis_log_t log = ldmsd_plug_log_get(handle);
    chrono_ldms_config cfg;
    char error[256] = "";
    char* durability = av_value(avl, "durability");
    int rc = 0;

    (void)kwl;
    memset(&cfg, 0, sizeof(cfg));
    cfg.catalog_endpoint = av_value(avl, "catalog");
    cfg.player_endpoint = av_value(avl, "player");
    cfg.queue_capacity = uint_attr(avl, "queue");
    cfg.batch_size = uint_attr(avl, "batch");
    cfg.flush_interval_ms = uint_attr(avl, "flush_ms");
    cfg.durability = (durability && strcmp(durability, "accepted") == 0) ? CHRONO_LDMS_ACCEPTED : CHRONO_LDMS_DURABLE;
    cfg.log = bridge_log;
    cfg.log_ctx = log;

    pthread_mutex_lock(&lock);
    if (bridge)
        ovis_log(log, OVIS_LWARN, PNAME ": already configured, keeping the first endpoint\n");
    else if (chrono_ldms_open(&cfg, &bridge, error, sizeof(error)) != CHRONO_LDMS_OK) {
        ovis_log(log, OVIS_LERROR, PNAME ": open failed: %s\n", error);
        rc = EINVAL;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

static ldmsd_store_handle_t open_store(ldmsd_plug_handle_t handle,
                                       const char* container,
                                       const char* schema,
                                       struct ldmsd_strgp_metric_list* metric_list)
{
    struct store_handle* sh;

    (void)schema;
    (void)metric_list;
    if (!container || !bridge) {
        ovis_log(ldmsd_plug_log_get(handle), OVIS_LERROR, PNAME ": open needs container= and a configured plugin\n");
        return NULL;
    }
    sh = calloc(1, sizeof(*sh));
    if (!sh)
        return NULL;
    sh->container = strdup(container);
    return (ldmsd_store_handle_t)sh;
}

static int scalar(ldms_set_t set, int idx, chrono_ldms_metric* out)
{
    switch (ldms_metric_type_get(set, idx)) {
    case LDMS_V_U8: out->kind = CHRONO_LDMS_U64; out->v.u64 = ldms_metric_get_u8(set, idx); return 1;
    case LDMS_V_U16: out->kind = CHRONO_LDMS_U64; out->v.u64 = ldms_metric_get_u16(set, idx); return 1;
    case LDMS_V_U32: out->kind = CHRONO_LDMS_U64; out->v.u64 = ldms_metric_get_u32(set, idx); return 1;
    case LDMS_V_U64: out->kind = CHRONO_LDMS_U64; out->v.u64 = ldms_metric_get_u64(set, idx); return 1;
    case LDMS_V_S8: out->kind = CHRONO_LDMS_I64; out->v.i64 = ldms_metric_get_s8(set, idx); return 1;
    case LDMS_V_S16: out->kind = CHRONO_LDMS_I64; out->v.i64 = ldms_metric_get_s16(set, idx); return 1;
    case LDMS_V_S32: out->kind = CHRONO_LDMS_I64; out->v.i64 = ldms_metric_get_s32(set, idx); return 1;
    case LDMS_V_S64: out->kind = CHRONO_LDMS_I64; out->v.i64 = ldms_metric_get_s64(set, idx); return 1;
    case LDMS_V_F32: out->kind = CHRONO_LDMS_F64; out->v.f64 = ldms_metric_get_float(set, idx); return 1;
    case LDMS_V_D64: out->kind = CHRONO_LDMS_F64; out->v.f64 = ldms_metric_get_double(set, idx); return 1;
    default: return 0;
    }
}

static int store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t _sh, ldms_set_t set, int* metric_arry, size_t metric_count)
{
    struct store_handle* sh = _sh;
    chrono_ldms_sample sample;
    chrono_ldms_metric* metrics;
    struct ldms_timestamp ts;
    size_t i, n = 0;
    int cid, rc;

    (void)handle;
    if (!sh || !bridge)
        return EINVAL;
    metrics = calloc(metric_count ? metric_count : 1, sizeof(*metrics));
    if (!metrics)
        return ENOMEM;
    for (i = 0; i < metric_count; i++) {
        int idx = metric_arry[i];
        metrics[n].name = ldms_metric_name_get(set, idx);
        if (scalar(set, idx, &metrics[n]))
            n++;
    }
    ts = ldms_transaction_timestamp_get(set);
    cid = ldms_metric_by_name(set, "component_id");
    memset(&sample, 0, sizeof(sample));
    sample.producer = ldms_set_producer_name_get(set);
    sample.instance = ldms_set_instance_name_get(set);
    sample.schema = ldms_set_schema_name_get(set);
    sample.component_id = cid >= 0 ? ldms_metric_get_u64(set, cid) : 0;
    sample.timestamp_ns = (uint64_t)ts.sec * 1000000000ull + (uint64_t)ts.usec * 1000ull;
    sample.metrics = metrics;
    sample.metric_count = n;
    rc = chrono_ldms_store_sample(bridge, sh->container, &sample);
    free(metrics);
    /* A full queue is counted by the bridge; ldmsd must not treat it as a store failure. */
    return rc == CHRONO_LDMS_OK || rc == CHRONO_LDMS_DROPPED ? 0 : EINVAL;
}

static int flush_store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t _sh)
{
    (void)handle;
    (void)_sh;
    return bridge && chrono_ldms_flush(bridge, 2000) == CHRONO_LDMS_OK ? 0 : EAGAIN;
}

static void close_store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t _sh)
{
    struct store_handle* sh = _sh;

    (void)handle;
    if (!sh)
        return;
    free(sh->container);
    free(sh);
}

static int constructor(ldmsd_plug_handle_t handle)
{
    (void)handle;
    pthread_mutex_lock(&lock);
    instances++;
    pthread_mutex_unlock(&lock);
    return 0;
}

static void destructor(ldmsd_plug_handle_t handle)
{
    (void)handle;
    pthread_mutex_lock(&lock);
    if (instances && --instances == 0 && bridge) {
        chrono_ldms_close(bridge, 5000);
        bridge = NULL;
    }
    pthread_mutex_unlock(&lock);
}

struct ldmsd_store ldmsd_plugin_interface = {
        .base =
                {
                        .type = LDMSD_PLUGIN_STORE,
                        .config = config,
                        .usage = usage,
                        .constructor = constructor,
                        .destructor = destructor,
                },
        .open = open_store,
        .close = close_store,
        .flush = flush_store,
        .store = store,
};
