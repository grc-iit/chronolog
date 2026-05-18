#include "Transform.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <string>


static inline std::string trim_copy(const std::string& s)
{
    auto b = s.begin();
    auto e = s.end();
    while(b != e && std::isspace(static_cast<unsigned char>(*b))) ++b;
    while(e != b && std::isspace(static_cast<unsigned char>(*(e - 1)))) --e;
    return std::string(b, e);
}

static bool parse_single_number(const std::string& payload, double& out)
{
    if(payload.empty())
        return false;

    {
        static const std::regex json_val_re(R"("value"\s*:\s*([+-]?\d*\.?\d+(?:[eE][+-]?\d+)?))");
        std::smatch m;
        if(std::regex_search(payload, m, json_val_re))
        {
            try
            {
                out = std::stod(m[1].str());
                return true;
            }
            catch(...)
            {}
        }
    }

    {
        static const std::regex kv_re(R"(value\s*=\s*([+-]?\d*\.?\d+(?:[eE][+-]?\d+)?))");
        std::smatch m;
        if(std::regex_search(payload, m, kv_re))
        {
            try
            {
                out = std::stod(m[1].str());
                return true;
            }
            catch(...)
            {}
        }
    }

    {
        static const std::regex any_num_re(R"([+-]?\d*\.?\d+(?:[eE][+-]?\d+)?)");
        std::smatch m;
        if(std::regex_search(payload, m, any_num_re))
        {
            try
            {
                out = std::stod(m[0].str());
                return true;
            }
            catch(...)
            {}
        }
    }

    std::string t = trim_copy(payload);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
    if(t == "true")
    {
        out = 1.0;
        return true;
    }
    else if(t == "false")
    {
        out = 0.0;
        return true;
    }

    return false;
}

static bool parse_two_numbers(const std::string& payload, double& a, double& b)
{
    auto comma = payload.find(',');
    if(comma != std::string::npos)
    {
        try
        {
            std::string s1 = trim_copy(payload.substr(0, comma));
            std::string s2 = trim_copy(payload.substr(comma + 1));
            a = std::stod(s1);
            b = std::stod(s2);
            return true;
        }
        catch(...)
        {}
    }
    static const std::regex any_num_re(R"([+-]?\d*\.?\d+(?:[eE][+-]?\d+)?)");
    std::sregex_iterator it(payload.begin(), payload.end(), any_num_re), end;
    if(it == end)
        return false;
    try
    {
        a = std::stod((*it).str());
        ++it;
        if(it == end)
            return false;
        b = std::stod((*it).str());
        return true;
    }
    catch(...)
    {}
    return false;
}

TelemetryBatch
toTelemetry(const std::vector<chronolog::Event>& events, const std::string& chronicle, const std::string& story)
{
    TelemetryBatch batch;
    batch.points.reserve(events.size());
    const bool is_network = (story == "network_usage");

    for(const auto& ev: events)
    {
        TelemetryPoint p;
        p.measurement = story;
        p.tags = {{"chronicle", chronicle}, {"story", story}};

        const std::string rec = ev.log_record();

        if(is_network)
        {
            double rx = 0.0, tx = 0.0;
            if(parse_two_numbers(rec, rx, tx))
            {
                p.fields = {{"rx_bytes", rx}, {"tx_bytes", tx}};
            }
            else
            {
                double v = 0.0;
                if(parse_single_number(rec, v))
                    p.fields = {{"rx_bytes", v}};
                else
                    p.fields = {{"rx_bytes", 0.0}, {"tx_bytes", 0.0}};
            }
        }
        else
        {
            double value = 0.0;
            if(!parse_single_number(rec, value))
                value = 0.0;
            p.fields = {{"value", value}};
        }

        p.ts_ns = ev.time();
        batch.points.push_back(std::move(p));
    }

    return batch;
}
