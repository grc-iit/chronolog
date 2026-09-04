/* -*- c-basic-offset: 8 -*-
 * store_chronolog.c -- LDMS storage plugin that writes metric-set samples
 * into ChronoLog as time-series "events".
 *
 * Mapping:
 *      LDMS strgp container       ->  ChronoLog Chronicle
 *      strgp schema + producer    ->  ChronoLog Story ("<schema>_<producer>")
 *      one LDMS sample            ->  one ChronoLog Event (a JSON line)
 *
 * The story is keyed per producer, not per storage policy: one strgp covers
 * every producer feeding its container/schema, and funnelling them through a
 * single story handle would serialise the aggregator's concurrency behind one
 * mutex. See clog_store() in chronolog_bridge.cpp.
 *
 * This file is the C ldmsd glue (ldmsd.h is not C++-safe). All ChronoLog and
 * serialization work lives in the C++ chronolog_bridge translation unit, which
 * this glue drives through the plain-C ABI in chronolog_bridge.h.
 *
 * This software is available to you under a choice of one of two licenses (GPL
 * v2 or the BSD-type license) in the same manner as the rest of the LDMS store
 * plugins; see the LICENSE / COPYING files at the root of the OVIS tree.
 */

#include <errno.h>
#include <string.h>

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"

#include "chronolog_bridge.h"

#define PNAME "store_chronolog"

static const char* usage(ldmsd_plug_handle_t handle)
{
    return "    config name=<inst> [client_conf=<path>]\n"
           "        Store LDMS metric sets into ChronoLog. chronicle=container and\n"
           "        story=<schema>_<producer>, so each producer feeding the strgp gets\n"
           "        its own story; each sample becomes a JSON event logged to it.\n"
           "        client_conf  Path to a ChronoLog client config JSON describing the\n"
           "                     ChronoVisor portal to connect to. Optional; if omitted\n"
           "                     the ChronoLog client defaults (127.0.0.1:5555) apply.\n";
}

static int config(ldmsd_plug_handle_t handle, struct attr_value_list* kwl, struct attr_value_list* avl)
{
    ovis_log_t log = ldmsd_plug_log_get(handle);
    char* conf_path = av_value(avl, "client_conf");

    /* Connect eagerly so connection errors surface at config time. */
    int rc = clog_connect(conf_path);
    if(rc)
    {
        ovis_log(log, OVIS_LERROR, PNAME ": connect failed: %s\n", clog_last_error());
        return rc;
    }
    ovis_log(log,
             OVIS_LINFO,
             PNAME ": connected to ChronoLog%s%s\n",
             conf_path ? " using " : " (defaults)",
             conf_path ? conf_path : "");
    return 0;
}

static ldmsd_store_handle_t open_store(ldmsd_plug_handle_t handle,
                                       const char* container,
                                       const char* schema,
                                       struct ldmsd_strgp_metric_list* metric_list)
{
    ovis_log_t log = ldmsd_plug_log_get(handle);
    clog_store_t s;

    if(!container || !schema)
    {
        ovis_log(log, OVIS_LERROR, PNAME ": open requires both container= and schema=\n");
        return NULL;
    }

    /* In case open() is reached without config (use defaults). */
    if(clog_connect(NULL))
    {
        ovis_log(log, OVIS_LERROR, PNAME ": connect failed: %s\n", clog_last_error());
        return NULL;
    }

    s = clog_open(container, schema);
    if(!s)
    {
        ovis_log(log, OVIS_LERROR, PNAME ": open '%s/%s' failed: %s\n", container, schema, clog_last_error());
        return NULL;
    }
    /* Not "opened story": clog_open() has no producer to work with, so it only
	 * records the chronicle/schema. The stories themselves are acquired lazily,
	 * one per producer, on that producer's first sample. */
    ovis_log(log, OVIS_LINFO, PNAME ": opened store for '%s/%s'\n", container, schema);
    return (ldmsd_store_handle_t)s;
}

static int
store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t _sh, ldms_set_t set, int* metric_arry, size_t metric_count)
{
    if(!_sh)
        return EINVAL;
    return clog_store((clog_store_t)_sh, set, metric_arry, metric_count);
}

static int flush_store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t _sh)
{
    /* ChronoLog flushes events on the keeper/grapher side; the client
	 * exposes no flush primitive, so this is a no-op. */
    return 0;
}

static void close_store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t _sh)
{
    if(!_sh)
        return;
    clog_close((clog_store_t)_sh);
    ovis_log(ldmsd_plug_log_get(handle), OVIS_LINFO, PNAME ": closed a store (releasing its per-producer stories)\n");
}

static int constructor(ldmsd_plug_handle_t handle)
{
    (void)handle;
    clog_plugin_attach();
    return 0;
}

/* Only the LAST instance's detach tears down the shared connection; see
 * clog_plugin_attach/detach. This used to call the process-wide clog_disconnect()
 * unconditionally, so terminating one instance freed the client and every
 * StoryHandle while another instance's open stories still pointed at them. */
static void destructor(ldmsd_plug_handle_t handle)
{
    (void)handle;
    clog_plugin_detach();
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
