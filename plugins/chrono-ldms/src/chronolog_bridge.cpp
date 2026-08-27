/*
 * chronolog_bridge.cpp -- C++ side of store_chronolog: owns the ChronoLog
 * client connection, the per-story handles, and the LDMS-set -> JSON
 * serialization. Exposes the plain-C ABI declared in chronolog_bridge.h so the
 * C ldmsd plugin glue can drive it without touching any C++ types.
 */

#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <new>
#include <cerrno>

#include "chronolog_bridge.h" /* pulls in ldms.h (C++-safe) */

#include <chronolog_client.h>
#include <ClientConfiguration.h>

namespace
{

/* Shared, process-wide ChronoLog connection state. */
struct Shared
{
    std::mutex lock;
    chronolog::Client* client = nullptr;
    bool connected = false;
    /* Number of live ldmsd plugin instances (constructor/destructor pairs). The
     * newer OVIS plug API is per-instance, but everything below is process-wide,
     * so one instance's destructor must not tear down state another still uses.
     * The connection is dropped only when the last instance detaches. */
    unsigned instances = 0;
    /* Acquired stories, keyed by chronicle + '/' + story, each refcounted.
     * ChronoLog's AcquireStory returns the SAME StoryHandle* for a repeat
     * acquisition and ReleaseStory deletes it outright
     * (StorytellerClient::removeAcquiredStoryHandle), so without a refcount two
     * storage policies over one container/schema share a handle and the first
     * strgp_stop frees it under the second. */
    std::map<std::string, struct Story*> stories;
    /* The portal the client was constructed with. A ChronoLog client is a
     * process-wide singleton whose endpoint is fixed at first construction
     * (ChronologClientImpl::GetClientImplInstance returns the existing instance
     * and ignores the portal argument), so this is the endpoint this process is
     * committed to for its lifetime. Kept so a later, conflicting client_conf
     * can be rejected loudly instead of silently ignored. */
    chronolog::ClientPortalServiceConf portal;
};
Shared g_shared;

/* Per-story handle returned to the C glue as an opaque clog_story_t. Shared by
 * every storage policy writing the same chronicle/story, hence the refcount. */
struct Story
{
    std::mutex lock;
    std::string key;
    std::string chronicle;
    std::string story;
    chronolog::StoryHandle* handle = nullptr;
    unsigned refs = 0; /* guarded by g_shared.lock, not by Story::lock */
};

thread_local std::string t_last_error;

void set_error(const std::string& msg) { t_last_error = msg; }

/* Endpoint identity for the client-portal conf, used to detect an operator
 * asking for an endpoint this process can no longer switch to. */
bool same_portal(chronolog::ClientPortalServiceConf const& a, chronolog::ClientPortalServiceConf const& b)
{
    return a.PROTO_CONF == b.PROTO_CONF && a.IP == b.IP && a.PORT == b.PORT && a.PROVIDER_ID == b.PROVIDER_ID;
}

std::string portal_to_string(chronolog::ClientPortalServiceConf const& p)
{
    return p.PROTO_CONF + "://" + p.IP + ":" + std::to_string(p.PORT) + "@" + std::to_string(p.PROVIDER_ID);
}

/* ---- JSON serialization ---------------------------------------------- */

/* Emit a JSON string.
 *
 * max_len bounds how far the source may be read. An LDMS char-array metric is a
 * fixed-size buffer that is NOT guaranteed NUL-terminated: a sampler writing a
 * string that exactly fills the declared size leaves no terminator, and scanning
 * for one then walks into the following metric's bytes (or past the set) until a
 * zero byte happens to appear, emitting adjacent data into the event. Callers
 * with a NUL-terminated C string (metric names, single chars) pass the default
 * and get the old behaviour. */
void json_string(std::ostringstream& out, const char* s, size_t max_len = SIZE_MAX)
{
    out << '"';
    if(s)
    {
        for(const char* p = s; (size_t)(p - s) < max_len && *p; ++p)
        {
            unsigned char c = (unsigned char)*p;
            switch(c)
            {
                case '"':
                    out << "\\\"";
                    break;
                case '\\':
                    out << "\\\\";
                    break;
                case '\n':
                    out << "\\n";
                    break;
                case '\r':
                    out << "\\r";
                    break;
                case '\t':
                    out << "\\t";
                    break;
                default:
                    if(c < 0x20)
                    {
                        out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec
                            << std::setfill(' ');
                    }
                    else
                    {
                        out << (char)c;
                    }
            }
        }
    }
    out << '"';
}

/* JSON has no NaN or Infinity literal. Streaming a non-finite double emits the
 * bare tokens `nan` / `-nan` / `inf`, which makes the whole event unparseable --
 * and because ChronoLog stores events as opaque strings nothing rejects it at
 * write time, so the sample is corrupt forever. Non-finite values become null,
 * which every JSON reader accepts. Derived/rate samplers produce these routinely
 * (first sample, division by zero). */
void json_number(std::ostringstream& out, double v, int precision)
{
    if(!std::isfinite(v))
    {
        out << "null";
        return;
    }
    out << std::setprecision(precision) << v;
}

void json_scalar(std::ostringstream& out, ldms_set_t set, int idx, enum ldms_value_type type)
{
    switch(type)
    {
        case LDMS_V_CHAR:
        {
            char buf[2] = {ldms_metric_get_char(set, idx), 0};
            json_string(out, buf);
            break;
        }
        case LDMS_V_U8:
            out << (unsigned)ldms_metric_get_u8(set, idx);
            break;
        case LDMS_V_S8:
            out << (int)ldms_metric_get_s8(set, idx);
            break;
        case LDMS_V_U16:
            out << ldms_metric_get_u16(set, idx);
            break;
        case LDMS_V_S16:
            out << ldms_metric_get_s16(set, idx);
            break;
        case LDMS_V_U32:
            out << ldms_metric_get_u32(set, idx);
            break;
        case LDMS_V_S32:
            out << ldms_metric_get_s32(set, idx);
            break;
        case LDMS_V_U64:
            out << ldms_metric_get_u64(set, idx);
            break;
        case LDMS_V_S64:
            out << ldms_metric_get_s64(set, idx);
            break;
        case LDMS_V_F32:
            json_number(out, ldms_metric_get_float(set, idx), 9);
            break;
        case LDMS_V_D64:
            json_number(out, ldms_metric_get_double(set, idx), 17);
            break;
        default:
            out << "null";
            break;
    }
}

void json_array(std::ostringstream& out, ldms_set_t set, int idx, enum ldms_value_type type)
{
    if(type == LDMS_V_CHAR_ARRAY)
    {
        /* Bounded by the metric's declared length -- the buffer need not be
         * NUL-terminated (see json_string). */
        json_string(out, ldms_metric_array_get_str(set, idx), (size_t)ldms_metric_array_get_len(set, idx));
        return;
    }
    int n = ldms_metric_array_get_len(set, idx);
    out << '[';
    for(int j = 0; j < n; j++)
    {
        if(j)
            out << ',';
        switch(type)
        {
            case LDMS_V_U8_ARRAY:
                out << (unsigned)ldms_metric_array_get_u8(set, idx, j);
                break;
            case LDMS_V_S8_ARRAY:
                out << (int)ldms_metric_array_get_s8(set, idx, j);
                break;
            case LDMS_V_U16_ARRAY:
                out << ldms_metric_array_get_u16(set, idx, j);
                break;
            case LDMS_V_S16_ARRAY:
                out << ldms_metric_array_get_s16(set, idx, j);
                break;
            case LDMS_V_U32_ARRAY:
                out << ldms_metric_array_get_u32(set, idx, j);
                break;
            case LDMS_V_S32_ARRAY:
                out << ldms_metric_array_get_s32(set, idx, j);
                break;
            case LDMS_V_U64_ARRAY:
                out << ldms_metric_array_get_u64(set, idx, j);
                break;
            case LDMS_V_S64_ARRAY:
                out << ldms_metric_array_get_s64(set, idx, j);
                break;
            case LDMS_V_F32_ARRAY:
                json_number(out, ldms_metric_array_get_float(set, idx, j), 9);
                break;
            case LDMS_V_D64_ARRAY:
                json_number(out, ldms_metric_array_get_double(set, idx, j), 17);
                break;
            default:
                out << "null";
                break;
        }
    }
    out << ']';
}

std::string serialize_set(ldms_set_t set, int* metric_arry, size_t metric_count)
{
    const struct ldms_timestamp ts = ldms_transaction_timestamp_get(set);
    std::ostringstream out;

    out << "{\"timestamp\":" << ts.sec << "." << std::setw(6) << std::setfill('0') << ts.usec << std::setfill(' ');
    out << ",\"producer\":";
    json_string(out, ldms_set_producer_name_get(set));
    out << ",\"instance\":";
    json_string(out, ldms_set_instance_name_get(set));
    out << ",\"schema\":";
    json_string(out, ldms_set_schema_name_get(set));
    out << ",\"metrics\":{";

    bool first = true;
    for(size_t i = 0; i < metric_count; i++)
    {
        int idx = metric_arry[i];
        enum ldms_value_type type = ldms_metric_type_get(set, idx);
        if(!first)
            out << ',';
        first = false;
        json_string(out, ldms_metric_name_get(set, idx));
        out << ':';
        if(type >= LDMS_V_CHAR && type <= LDMS_V_D64)
            json_scalar(out, set, idx, type);
        else if(type >= LDMS_V_CHAR_ARRAY && type <= LDMS_V_D64_ARRAY)
            json_array(out, set, idx, type);
        else
            out << "null"; /* LIST / RECORD not supported */
    }
    out << "}}";
    return out.str();
}

} /* anonymous namespace */

/* ---- C ABI ----------------------------------------------------------- */

extern "C"
{

    int clog_connect(const char* client_conf_path)
    {
        std::lock_guard<std::mutex> g(g_shared.lock);

        /* An explicit client_conf is an operator instruction and must never be
         * silently dropped. A NULL/empty path is the open_store() fallback
         * ("reached without config -- use defaults") and must NOT override an
         * endpoint an earlier config() already established. */
        const bool explicit_conf = (client_conf_path && client_conf_path[0]);

        chronolog::ClientConfiguration conf;
        if(explicit_conf && !conf.load_from_file(client_conf_path))
        {
            set_error(std::string("failed to load client_conf '") + client_conf_path + "'");
            return EINVAL;
        }

        chronolog::ClientPortalServiceConf portal;
        portal.PROTO_CONF = conf.PORTAL_CONF.PROTO_CONF;
        portal.IP = conf.PORTAL_CONF.IP;
        portal.PORT = conf.PORTAL_CONF.PORT;
        portal.PROVIDER_ID = conf.PORTAL_CONF.PROVIDER_ID;

        if(g_shared.client)
        {
            /* The endpoint is fixed for the life of the process (see Shared::portal).
             * If the operator is now asking for a different one, say so instead of
             * connecting to the old address and reporting success -- in a
             * distributed deployment that is the difference between reaching the
             * real ChronoVisor and silently talking to 127.0.0.1 forever. */
            if(explicit_conf && !same_portal(g_shared.portal, portal))
            {
                set_error("client_conf '" + std::string(client_conf_path) + "' requests " + portal_to_string(portal) +
                          " but this process is already bound to " + portal_to_string(g_shared.portal) +
                          "; the ChronoLog client endpoint cannot be changed once created. Configure "
                          "store_chronolog before starting any storage policy, and restart ldmsd to change it.");
                return EINVAL;
            }
            if(g_shared.connected)
                return 0;
            /* Client exists but the previous Connect() failed -- retry it below. */
        }
        else
        {
            g_shared.client = new(std::nothrow) chronolog::Client(portal);
            if(!g_shared.client)
            {
                set_error("out of memory creating ChronoLog client");
                return ENOMEM;
            }
            g_shared.portal = portal;
        }

        int rc = g_shared.client->Connect();
        if(rc != chronolog::CL_SUCCESS)
        {
            set_error("ChronoLog Connect() to " + portal_to_string(g_shared.portal) +
                      " failed rc=" + std::to_string(rc));
            return EIO;
        }
        g_shared.connected = true;
        return 0;
    }

    clog_story_t clog_open(const char* chronicle, const char* story)
    {
        std::lock_guard<std::mutex> g(g_shared.lock);
        if(!g_shared.connected || !g_shared.client)
        {
            set_error("clog_open before successful clog_connect");
            return nullptr;
        }

        /* Two storage policies over the same container/schema is an ordinary
         * ldmsd configuration, and both land here. Share one refcounted Story
         * rather than handing out two owners of one StoryHandle. */
        const std::string key = std::string(chronicle) + "/" + story;
        auto it = g_shared.stories.find(key);
        if(it != g_shared.stories.end())
        {
            ++it->second->refs;
            return it->second;
        }

        int rc = g_shared.client->CreateChronicle(chronicle);
        if(rc != chronolog::CL_SUCCESS && rc != chronolog::CL_ERR_CHRONICLE_EXISTS)
        {
            set_error(std::string("CreateChronicle('") + chronicle + "') failed rc=" + std::to_string(rc));
            return nullptr;
        }

        auto acq = g_shared.client->AcquireStory(chronicle, story);
        if(acq.first != chronolog::CL_SUCCESS || !acq.second)
        {
            set_error(std::string("AcquireStory('") + chronicle + "/" + story +
                      "') failed rc=" + std::to_string(acq.first));
            return nullptr;
        }

        Story* s = new(std::nothrow) Story();
        if(!s)
        {
            /* Safe to release here: the registry lookup above proved no other
             * Story owns this handle yet. */
            g_shared.client->ReleaseStory(chronicle, story);
            set_error("out of memory allocating story handle");
            return nullptr;
        }
        s->key = key;
        s->chronicle = chronicle;
        s->story = story;
        s->handle = acq.second;
        s->refs = 1;
        g_shared.stories.emplace(key, s);
        return s;
    }

    int clog_store(clog_story_t _s, ldms_set_t set, int* metric_arry, size_t metric_count)
    {
        Story* s = static_cast<Story*>(_s);
        if(!s || !s->handle)
            return EINVAL;

        std::string json = serialize_set(set, metric_arry, metric_count);

        std::lock_guard<std::mutex> g(s->lock);
        /* log_event() returns the event's timestamp on success and 0 on failure
         * -- both when no keeper could be chosen and when the send itself failed
         * (StoryHandleImpl::log_event). Swallowing that told ldmsd every sample
         * was stored, so a dead keeper silently discarded the whole metric
         * stream: no log, no retry, no strgp failure. */
        if(s->handle->log_event(json) == 0)
        {
            set_error("log_event failed for story '" + s->chronicle + "/" + s->story +
                      "' (no keeper available, or the send failed)");
            return EIO;
        }
        return 0;
    }

    void clog_close(clog_story_t _s)
    {
        Story* s = static_cast<Story*>(_s);
        if(!s)
            return;

        std::lock_guard<std::mutex> g(g_shared.lock);
        if(s->refs > 0 && --s->refs > 0)
        {
            /* Another storage policy is still writing this story. Releasing now
             * would delete the StoryHandle out from under it -- ReleaseStory
             * destroys the handle rather than dropping a reference. */
            return;
        }
        if(g_shared.client && g_shared.connected)
            g_shared.client->ReleaseStory(s->chronicle, s->story);
        g_shared.stories.erase(s->key);
        delete s;
    }

    void clog_plugin_attach(void)
    {
        std::lock_guard<std::mutex> g(g_shared.lock);
        ++g_shared.instances;
    }

    void clog_plugin_detach(void)
    {
        std::lock_guard<std::mutex> g(g_shared.lock);
        if(g_shared.instances > 0 && --g_shared.instances > 0)
        {
            /* Another plugin instance is still loaded. The client, the
             * connection and every acquired story are process-wide, so tearing
             * them down here would leave that instance's Story pointers
             * dangling on its next sample. */
            return;
        }
        /* Release anything still acquired before dropping the connection, so a
         * later reload re-acquires cleanly. Disconnect() itself does NOT release
         * stories (it only issues the visor RPC and marks the client
         * SHUTTING_DOWN), and ~StorytellerClient's handle-delete loop is
         * commented out, so entries left here would survive as stale registry
         * hits and clog_open() would hand back a handle for a story this process
         * no longer holds. */
        if(g_shared.client && g_shared.connected)
        {
            for(auto& entry: g_shared.stories)
            {
                g_shared.client->ReleaseStory(entry.second->chronicle, entry.second->story);
                delete entry.second;
            }
            g_shared.stories.clear();
            g_shared.client->Disconnect();
            g_shared.connected = false;
        }
        /* Deliberately NOT `delete g_shared.client`. ChronologClientImpl is a
         * process-wide singleton whose static instance pointer is never cleared
         * by its destructor, so deleting the Client leaves that static dangling
         * and the next clog_connect() -- after an ordinary plugin reload --
         * would get the freed object back from GetClientImplInstance(). Keeping
         * the object for the process lifetime costs one idle client; Connect()
         * accepts a reconnect from the disconnected state, so a reload works. */
    }

    const char* clog_last_error(void) { return t_last_error.c_str(); }

} /* extern "C" */
