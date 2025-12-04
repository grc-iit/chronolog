#pragma once
#include "StreamSink.h"
#include <chronolog_client.h>
#include <string>
#include <vector>

TelemetryBatch
toTelemetry(const std::vector<chronolog::Event>& events, const std::string& chronicle, const std::string& story);
