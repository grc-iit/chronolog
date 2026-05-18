#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct TelemetryPoint
{
    std::string measurement;
    std::map<std::string, std::string> tags;
    std::map<std::string, double> fields;
    uint64_t ts_ns;
};

struct TelemetryBatch
{
    std::vector<TelemetryPoint> points;
};

class StreamSink
{
public:
    virtual ~StreamSink() = default;
    virtual bool send(const TelemetryBatch& batch) = 0;
};
