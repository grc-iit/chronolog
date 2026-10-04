/* Stands in for ldmsd: makes the calls its store plugin interface makes (config, open, store from several
 * updater threads, flush, close) against the C bridge, so the bridge is tested without an LDMS install. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chronolog_ldms_bridge.h"

struct options
{
    const char* catalog;
    const char* player;
    const char* container;
    const char* schema;
    unsigned producers, samples, interval_us, stale_every, max_store_ms;
    chrono_ldms_config config;
    int allow_drops;
};

struct updater
{
    const struct options* options;
    chrono_ldms_t* bridge;
    unsigned index;
    uint64_t max_store_ns;
    uint64_t store_errors;
};

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void* updater_main(void* arg)
{
    struct updater* u = arg;
    char producer[64];
    char instance[96];
    unsigned i;

    snprintf(producer, sizeof(producer), "node%u.example.org", u->index);
    snprintf(instance, sizeof(instance), "%s/meminfo", producer);
    for (i = 0; i < u->options->samples; i++) {
        chrono_ldms_metric metrics[4];
        chrono_ldms_sample sample;
        uint64_t begin, spent;
        int rc;

        metrics[0].name = "MemTotal";
        metrics[0].kind = CHRONO_LDMS_U64;
        metrics[0].v.u64 = 16000000ull;
        metrics[1].name = "MemFree";
        metrics[1].kind = CHRONO_LDMS_U64;
        metrics[1].v.u64 = 8000000ull - i;
        metrics[2].name = "load";
        metrics[2].kind = CHRONO_LDMS_F64;
        metrics[2].v.f64 = 0.25 * (double)(i % 8);
        metrics[3].name = "delta";
        metrics[3].kind = CHRONO_LDMS_I64;
        metrics[3].v.i64 = -(int64_t)i;
        memset(&sample, 0, sizeof(sample));
        sample.producer = producer;
        sample.instance = instance;
        sample.schema = u->options->schema;
        sample.component_id = u->index + 1;
        sample.timestamp_ns = clock_ns(CLOCK_REALTIME);
        if (u->options->stale_every && (i + 1) % u->options->stale_every == 0)
            sample.timestamp_ns -= 3600ull * 1000000000ull;
        sample.metrics = metrics;
        sample.metric_count = 4;
        begin = clock_ns(CLOCK_MONOTONIC);
        rc = chrono_ldms_store_sample(u->bridge, u->options->container, &sample);
        spent = clock_ns(CLOCK_MONOTONIC) - begin;
        if (spent > u->max_store_ns)
            u->max_store_ns = spent;
        if (rc != CHRONO_LDMS_OK && rc != CHRONO_LDMS_DROPPED)
            u->store_errors++;
        if (u->options->interval_us) {
            struct timespec pause;
            pause.tv_sec = u->options->interval_us / 1000000;
            pause.tv_nsec = (long)(u->options->interval_us % 1000000) * 1000;
            nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

int main(int argc, char** argv)
{
    struct options o;
    chrono_ldms_t* bridge = NULL;
    chrono_ldms_stats stats;
    pthread_t threads[64];
    struct updater updaters[64];
    char error[256] = "";
    uint64_t max_store_ns = 0, store_errors = 0, total;
    unsigned i;
    int ok = 1;

    memset(&o, 0, sizeof(o));
    o.container = "ldms";
    o.schema = "meminfo";
    o.producers = 3;
    o.samples = 20;
    o.interval_us = 1000;
    o.max_store_ms = 100;
    for (i = 1; (int)i < argc; i++) {
        const char* key = argv[i];
        const char* value = (int)i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(key, "--allow-drops") == 0) {
            o.allow_drops = 1;
            continue;
        }
        if (!value) {
            fprintf(stderr, "missing value for %s\n", key);
            return 2;
        }
        i++;
        if (strcmp(key, "--catalog") == 0) o.catalog = value;
        else if (strcmp(key, "--player") == 0) o.player = value;
        else if (strcmp(key, "--container") == 0) o.container = value;
        else if (strcmp(key, "--schema") == 0) o.schema = value;
        else if (strcmp(key, "--producers") == 0) o.producers = (unsigned)atoi(value);
        else if (strcmp(key, "--samples") == 0) o.samples = (unsigned)atoi(value);
        else if (strcmp(key, "--interval-us") == 0) o.interval_us = (unsigned)atoi(value);
        else if (strcmp(key, "--stale-every") == 0) o.stale_every = (unsigned)atoi(value);
        else if (strcmp(key, "--max-store-ms") == 0) o.max_store_ms = (unsigned)atoi(value);
        else if (strcmp(key, "--queue") == 0) o.config.queue_capacity = (uint32_t)atoi(value);
        else if (strcmp(key, "--batch") == 0) o.config.batch_size = (uint32_t)atoi(value);
        else if (strcmp(key, "--flush-ms") == 0) o.config.flush_interval_ms = (uint32_t)atoi(value);
        else {
            fprintf(stderr, "unknown option %s\n", key);
            return 2;
        }
    }
    if (!o.catalog || o.producers == 0 || o.producers > 64) {
        fprintf(stderr, "usage: fake_ldmsd --catalog host:port [--player host:port] [--container c] [--schema s]\n"
                        "       [--producers n<=64] [--samples n] [--interval-us n] [--stale-every n] [--queue n]\n"
                        "       [--batch n] [--flush-ms n] [--max-store-ms n] [--allow-drops]\n");
        return 2;
    }
    o.config.catalog_endpoint = o.catalog;
    o.config.player_endpoint = o.player;
    if (chrono_ldms_open(&o.config, &bridge, error, sizeof(error)) != CHRONO_LDMS_OK) {
        fprintf(stderr, "open failed: %s\n", error);
        return 1;
    }
    for (i = 0; i < o.producers; i++) {
        updaters[i].options = &o;
        updaters[i].bridge = bridge;
        updaters[i].index = i;
        updaters[i].max_store_ns = 0;
        updaters[i].store_errors = 0;
        pthread_create(&threads[i], NULL, updater_main, &updaters[i]);
    }
    for (i = 0; i < o.producers; i++) {
        pthread_join(threads[i], NULL);
        if (updaters[i].max_store_ns > max_store_ns)
            max_store_ns = updaters[i].max_store_ns;
        store_errors += updaters[i].store_errors;
    }
    if (chrono_ldms_flush(bridge, 30000) != CHRONO_LDMS_OK)
        ok = 0;
    chrono_ldms_stats_get(bridge, &stats);
    if (chrono_ldms_close(bridge, 10000) != CHRONO_LDMS_OK)
        ok = 0;
    total = (uint64_t)o.producers * o.samples;
    for (i = 0; stats.last_error[i]; i++)
        if (stats.last_error[i] == '"' || stats.last_error[i] == '\\')
            stats.last_error[i] = '\'';
    printf("{\"samples\":%" PRIu64 ",\"appended\":%" PRIu64 ",\"dropped\":%" PRIu64 ",\"failed\":%" PRIu64
           ",\"queued\":%" PRIu64 ",\"max_store_us\":%" PRIu64 ",\"store_errors\":%" PRIu64 ",\"last_error\":\"%s\"}\n",
           total, stats.appended, stats.dropped, stats.failed, stats.queued, max_store_ns / 1000, store_errors,
           stats.last_error);
    if (stats.failed || store_errors || stats.queued || stats.appended + stats.dropped != total)
        ok = 0;
    if (!o.allow_drops && stats.dropped)
        ok = 0;
    if (o.allow_drops && stats.dropped == 0)
        ok = 0;
    if (max_store_ns > (uint64_t)o.max_store_ms * 1000000ull)
        ok = 0;
    return ok ? 0 : 1;
}
