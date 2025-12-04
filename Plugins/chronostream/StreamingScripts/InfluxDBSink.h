#pragma once
#include "StreamSink.h"
#include <string>
#include <vector>

class InfluxDBSink: public StreamSink
{
public:
    struct Options
    {
        std::string url;
        std::string token;
        size_t max_batch_points = 5000;
        int timeout_ms = 5000;
        int max_retries = 3;
    };

    explicit InfluxDBSink(Options opts);
    bool send(const TelemetryBatch& batch) override;

private:
    Options options;
    static std::string toLineProtocol(const TelemetryBatch& batch);
    static std::string escMeasurement(const std::string& s);
    static std::string escTagKeyVal(const std::string& s);
    static std::string escFieldKey(const std::string& s);
    static std::string toFieldValue(double v);
    bool httpPost(const std::string& payload);
};
