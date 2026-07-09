/*
 * bench_common.h -- shared payload generation + timing for the Kafka vs
 * ChronoLog storage-backend benchmark. Both backends are fed byte-identical,
 * LDMS-shaped JSON metric records so the comparison reflects the storage path,
 * not payload differences.
 */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <string>
#include <chrono>
#include <cstdio>

// Build an LDMS-style JSON metric record padded to approximately `size` bytes.
// Mirrors what store_chronolog emits: a timestamp/producer/instance/schema
// envelope plus a metrics object. A filler metric pads to the target size.
static inline std::string make_payload(size_t size)
{
	std::string head =
	    "{\"timestamp\":1782530790.001967,\"producer\":\"node1\","
	    "\"instance\":\"node1/meminfo\",\"schema\":\"meminfo\",\"metrics\":{"
	    "\"component_id\":1,\"MemTotal\":65467136,\"MemFree\":20060084,"
	    "\"MemAvailable\":42687428,\"Cached\":16026808,\"_pad\":\"";
	std::string tail = "\"}}";
	if (size <= head.size() + tail.size() + 1) {
		// Too small to hold the envelope: emit a minimal record.
		std::string s = "{\"v\":\"";
		while (s.size() + 2 < size) s.push_back('x');
		s += "\"}";
		return s;
	}
	size_t pad = size - head.size() - tail.size();
	std::string s;
	s.reserve(size);
	s += head;
	s.append(pad, 'x');
	s += tail;
	return s;
}

// Wall-clock epoch seconds: usable both for per-process durations and for
// aggregating overlapping timed regions across independent benchmark processes.
static inline double now_sec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(
		system_clock::now().time_since_epoch()).count();
}

#endif
