#include "InfluxDBSink.h"
#include <chrono>
#include <curl/curl.h>
#include <sstream>
#include <thread>

InfluxDBSink::InfluxDBSink(Options opts)
    : options(std::move(opts))
{
    curl_global_init(CURL_GLOBAL_ALL);
}

static inline void replaceAll(std::string& s, const char* from, const char* to)
{
    size_t pos = 0;
    const size_t flen = std::char_traits<char>::length(from);
    const size_t tlen = std::char_traits<char>::length(to);
    while((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, flen, to);
        pos += tlen;
    }
}

std::string InfluxDBSink::escMeasurement(const std::string& s)
{
    std::string out = s;
    replaceAll(out, " ", "\\ ");
    replaceAll(out, ",", "\\,");
    replaceAll(out, "=", "\\=");
    return out;
}
std::string InfluxDBSink::escTagKeyVal(const std::string& s)
{
    std::string out = s;
    replaceAll(out, " ", "\\ ");
    replaceAll(out, ",", "\\,");
    replaceAll(out, "=", "\\=");
    return out;
}
std::string InfluxDBSink::escFieldKey(const std::string& s)
{
    std::string out = s;
    replaceAll(out, " ", "\\ ");
    replaceAll(out, ",", "\\,");
    replaceAll(out, "=", "\\=");
    return out;
}
std::string InfluxDBSink::toFieldValue(double v)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << v;
    return oss.str();
}

std::string InfluxDBSink::toLineProtocol(const TelemetryBatch& batch)
{
    std::string payload;
    payload.reserve(batch.points.size() * 64);
    for(const auto& p: batch.points)
    {
        payload += escMeasurement(p.measurement);

        for(const auto& [k, v]: p.tags)
        {
            payload += ",";
            payload += escTagKeyVal(k);
            payload += "=";
            payload += escTagKeyVal(v);
        }

        payload += " ";
        bool first = true;
        for(const auto& [fk, fv]: p.fields)
        {
            if(!first)
                payload += ",";
            first = false;
            payload += escFieldKey(fk);
            payload += "=";
            payload += toFieldValue(fv);
        }
        payload += " ";
        payload += std::to_string(p.ts_ns);
        payload += "\n";
    }
    return payload;
}

bool InfluxDBSink::httpPost(const std::string& payload)
{
    CURL* curl = curl_easy_init();
    if(!curl)
        return false;

    struct curl_slist* headers = nullptr;
    std::string auth = "Authorization: Token " + options.token;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_URL, options.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, options.timeout_ms);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && code >= 200 && code < 300);
}

bool InfluxDBSink::send(const TelemetryBatch& batch)
{
    if(batch.points.empty())
        return true;
    const std::string body = toLineProtocol(batch);

    int attempt = 0;
    while(attempt++ <= options.max_retries)
    {
        if(httpPost(body))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
    }
    return false;
}
