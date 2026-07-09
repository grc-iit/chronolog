/*
 * chronolog_bridge.cpp -- C++ side of store_chronolog: owns the ChronoLog
 * client connection, the per-story handles, and the LDMS-set -> JSON
 * serialization. Exposes the plain-C ABI declared in chronolog_bridge.h so the
 * C ldmsd plugin glue can drive it without touching any C++ types.
 */

#include <string>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <new>
#include <cerrno>

#include "chronolog_bridge.h"	/* pulls in ldms.h (C++-safe) */

#include <chronolog_client.h>
#include <ClientConfiguration.h>

namespace {

/* Shared, process-wide ChronoLog connection state. */
struct Shared {
	std::mutex			lock;
	chronolog::Client		*client = nullptr;
	bool				connected = false;
};
Shared g_shared;

/* Per-story handle returned to the C glue as an opaque clog_story_t. */
struct Story {
	std::mutex		lock;
	std::string		chronicle;
	std::string		story;
	chronolog::StoryHandle	*handle = nullptr;
};

thread_local std::string t_last_error;

void set_error(const std::string &msg) { t_last_error = msg; }

/* ---- JSON serialization ---------------------------------------------- */

void json_string(std::ostringstream &out, const char *s)
{
	out << '"';
	if (s) {
		for (const char *p = s; *p; ++p) {
			unsigned char c = (unsigned char)*p;
			switch (c) {
			case '"':  out << "\\\""; break;
			case '\\': out << "\\\\"; break;
			case '\n': out << "\\n";  break;
			case '\r': out << "\\r";  break;
			case '\t': out << "\\t";  break;
			default:
				if (c < 0x20) {
					out << "\\u" << std::hex << std::setw(4)
					    << std::setfill('0') << (int)c
					    << std::dec << std::setfill(' ');
				} else {
					out << (char)c;
				}
			}
		}
	}
	out << '"';
}

void json_scalar(std::ostringstream &out, ldms_set_t set, int idx,
		 enum ldms_value_type type)
{
	switch (type) {
	case LDMS_V_CHAR: {
		char buf[2] = { ldms_metric_get_char(set, idx), 0 };
		json_string(out, buf);
		break;
	}
	case LDMS_V_U8:  out << (unsigned)ldms_metric_get_u8(set, idx);  break;
	case LDMS_V_S8:  out << (int)ldms_metric_get_s8(set, idx);       break;
	case LDMS_V_U16: out << ldms_metric_get_u16(set, idx);           break;
	case LDMS_V_S16: out << ldms_metric_get_s16(set, idx);           break;
	case LDMS_V_U32: out << ldms_metric_get_u32(set, idx);           break;
	case LDMS_V_S32: out << ldms_metric_get_s32(set, idx);           break;
	case LDMS_V_U64: out << ldms_metric_get_u64(set, idx);           break;
	case LDMS_V_S64: out << ldms_metric_get_s64(set, idx);           break;
	case LDMS_V_F32:
		out << std::setprecision(9) << ldms_metric_get_float(set, idx);
		break;
	case LDMS_V_D64:
		out << std::setprecision(17) << ldms_metric_get_double(set, idx);
		break;
	default:
		out << "null";
		break;
	}
}

void json_array(std::ostringstream &out, ldms_set_t set, int idx,
		enum ldms_value_type type)
{
	if (type == LDMS_V_CHAR_ARRAY) {
		json_string(out, ldms_metric_array_get_str(set, idx));
		return;
	}
	int n = ldms_metric_array_get_len(set, idx);
	out << '[';
	for (int j = 0; j < n; j++) {
		if (j)
			out << ',';
		switch (type) {
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
			out << std::setprecision(9)
			    << ldms_metric_array_get_float(set, idx, j);
			break;
		case LDMS_V_D64_ARRAY:
			out << std::setprecision(17)
			    << ldms_metric_array_get_double(set, idx, j);
			break;
		default:
			out << "null";
			break;
		}
	}
	out << ']';
}

std::string serialize_set(ldms_set_t set, int *metric_arry, size_t metric_count)
{
	const struct ldms_timestamp ts = ldms_transaction_timestamp_get(set);
	std::ostringstream out;

	out << "{\"timestamp\":" << ts.sec << "."
	    << std::setw(6) << std::setfill('0') << ts.usec
	    << std::setfill(' ');
	out << ",\"producer\":";
	json_string(out, ldms_set_producer_name_get(set));
	out << ",\"instance\":";
	json_string(out, ldms_set_instance_name_get(set));
	out << ",\"schema\":";
	json_string(out, ldms_set_schema_name_get(set));
	out << ",\"metrics\":{";

	bool first = true;
	for (size_t i = 0; i < metric_count; i++) {
		int idx = metric_arry[i];
		enum ldms_value_type type = ldms_metric_type_get(set, idx);
		if (!first)
			out << ',';
		first = false;
		json_string(out, ldms_metric_name_get(set, idx));
		out << ':';
		if (type >= LDMS_V_CHAR && type <= LDMS_V_D64)
			json_scalar(out, set, idx, type);
		else if (type >= LDMS_V_CHAR_ARRAY && type <= LDMS_V_D64_ARRAY)
			json_array(out, set, idx, type);
		else
			out << "null"; /* LIST / RECORD not supported */
	}
	out << "}}";
	return out.str();
}

} /* anonymous namespace */

/* ---- C ABI ----------------------------------------------------------- */

extern "C" {

int clog_connect(const char *client_conf_path)
{
	std::lock_guard<std::mutex> g(g_shared.lock);
	if (g_shared.connected)
		return 0;

	chronolog::ClientConfiguration conf;
	if (client_conf_path && client_conf_path[0]) {
		if (!conf.load_from_file(client_conf_path)) {
			set_error(std::string("failed to load client_conf '") +
				  client_conf_path + "'");
			return EINVAL;
		}
	}

	chronolog::ClientPortalServiceConf portal;
	portal.PROTO_CONF  = conf.PORTAL_CONF.PROTO_CONF;
	portal.IP          = conf.PORTAL_CONF.IP;
	portal.PORT        = conf.PORTAL_CONF.PORT;
	portal.PROVIDER_ID = conf.PORTAL_CONF.PROVIDER_ID;

	if (!g_shared.client) {
		g_shared.client = new(std::nothrow) chronolog::Client(portal);
		if (!g_shared.client) {
			set_error("out of memory creating ChronoLog client");
			return ENOMEM;
		}
	}

	int rc = g_shared.client->Connect();
	if (rc != chronolog::CL_SUCCESS) {
		set_error("ChronoLog Connect() failed rc=" + std::to_string(rc));
		return EIO;
	}
	g_shared.connected = true;
	return 0;
}

clog_story_t clog_open(const char *chronicle, const char *story)
{
	std::lock_guard<std::mutex> g(g_shared.lock);
	if (!g_shared.connected || !g_shared.client) {
		set_error("clog_open before successful clog_connect");
		return nullptr;
	}

	int rc = g_shared.client->CreateChronicle(chronicle);
	if (rc != chronolog::CL_SUCCESS &&
	    rc != chronolog::CL_ERR_CHRONICLE_EXISTS) {
		set_error(std::string("CreateChronicle('") + chronicle +
			  "') failed rc=" + std::to_string(rc));
		return nullptr;
	}

	auto acq = g_shared.client->AcquireStory(chronicle, story);
	if (acq.first != chronolog::CL_SUCCESS || !acq.second) {
		set_error(std::string("AcquireStory('") + chronicle + "/" +
			  story + "') failed rc=" + std::to_string(acq.first));
		return nullptr;
	}

	Story *s = new(std::nothrow) Story();
	if (!s) {
		g_shared.client->ReleaseStory(chronicle, story);
		set_error("out of memory allocating story handle");
		return nullptr;
	}
	s->chronicle = chronicle;
	s->story = story;
	s->handle = acq.second;
	return s;
}

int clog_store(clog_story_t _s, ldms_set_t set,
	       int *metric_arry, size_t metric_count)
{
	Story *s = static_cast<Story *>(_s);
	if (!s || !s->handle)
		return EINVAL;

	std::string json = serialize_set(set, metric_arry, metric_count);

	std::lock_guard<std::mutex> g(s->lock);
	s->handle->log_event(json);
	return 0;
}

void clog_close(clog_story_t _s)
{
	Story *s = static_cast<Story *>(_s);
	if (!s)
		return;
	{
		std::lock_guard<std::mutex> g(g_shared.lock);
		if (g_shared.client && g_shared.connected)
			g_shared.client->ReleaseStory(s->chronicle, s->story);
	}
	delete s;
}

void clog_disconnect(void)
{
	std::lock_guard<std::mutex> g(g_shared.lock);
	if (g_shared.client) {
		if (g_shared.connected)
			g_shared.client->Disconnect();
		delete g_shared.client;
		g_shared.client = nullptr;
		g_shared.connected = false;
	}
}

const char *clog_last_error(void)
{
	return t_last_error.c_str();
}

} /* extern "C" */
