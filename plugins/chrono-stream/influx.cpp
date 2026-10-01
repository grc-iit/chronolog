#include "chronolog/stream/stream.h"
#include <cmath>
#include <curl/curl.h>
#include <iomanip>
#include <sstream>
#include <thread>

namespace chronolog::stream
{
namespace
{
std::string escape(const std::string& value, bool equals)
{
    std::string result;
    for(unsigned char c: value)
    {
        if(c < ' ' || c == 127)
            throw std::invalid_argument("line protocol identifiers cannot contain control characters");
        if(c == ' ' || c == ',' || c == '\\' || (equals && c == '='))
            result += '\\';
        result += static_cast<char>(c);
    }
    if(result.empty())
        throw std::invalid_argument("empty line protocol identifier");
    return result;
}
size_t discard(char*, size_t size, size_t count, void*) { return size * count; }
bool retryable(CURLcode code)
{
    return code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_RESOLVE_PROXY ||
           code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR || code == CURLE_OPERATION_TIMEDOUT ||
           code == CURLE_GOT_NOTHING;
}
} // namespace
absl::StatusOr<std::string> lineProtocol(const Event& event)
{
    if(event.envelope.content_type != "application/vnd.chronolog.metric+json")
        return absl::InvalidArgumentError("event is not a metric");
    try
    {
        auto metric = Json::parse(event.envelope.payload);
        std::string line = escape(metric.at("name").get<std::string>(), false);
        auto host = event.envelope.attributes.find("host.name");
        if(host == event.envelope.attributes.end())
            return absl::InvalidArgumentError("metric needs host.name");
        std::map<std::string, std::string> labels = metric.value("labels", std::map<std::string, std::string>{});
        labels["host.name"] = host->second;
        for(const auto& [key, value]: labels) line += "," + escape(key, true) + "=" + escape(value, true);
        auto fields = metric.contains("values") ? metric.at("values") : Json{{"value", metric.at("value")}};
        if(!fields.is_object() || fields.empty() || fields.size() > 256)
            return absl::InvalidArgumentError("metric fields must be a nonempty numeric object");
        line += ' ';
        bool first = true;
        for(const auto& [key, value]: fields.items())
        {
            if(!value.is_number() || !std::isfinite(value.get<double>()))
                return absl::InvalidArgumentError("metric field is not finite numeric");
            if(!first)
                line += ',';
            first = false;
            line += escape(key, true) + "=";
            if(value.is_number_unsigned())
                line += std::to_string(value.get<uint64_t>()) + "u";
            else if(value.is_number_integer())
                line += std::to_string(value.get<int64_t>()) + "i";
            else
            {
                std::ostringstream number;
                number.imbue(std::locale::classic());
                number << std::setprecision(17) << value.get<double>();
                line += number.str();
            }
        }
        line += ' ';
        line += std::to_string(event.physical.physical_ns);
        line += '\n';
        return line;
    }
    catch(const std::exception& e)
    {
        return absl::InvalidArgumentError(e.what());
    }
}
absl::Status InfluxSink::post(const std::string& payload, client::Deadline deadline)
{
    if(payload.empty())
        return absl::OkStatus();
    if(payload.size() > (1u << 20) || options_.retries > 8 || options_.timeout.count() <= 0 ||
       options_.timeout > std::chrono::seconds(30) || options_.backoff.count() < 0 ||
       options_.max_backoff.count() < 0 || options_.max_backoff > std::chrono::seconds(5))
        return absl::InvalidArgumentError("invalid Influx retry, timeout or payload bounds");
    if(options_.url.rfind("http://", 0) != 0 && options_.url.rfind("https://", 0) != 0)
        return absl::InvalidArgumentError("Influx URL must be HTTP(S)");
    if(options_.token.find_first_of("\r\n") != std::string::npos)
        return absl::InvalidArgumentError("invalid Influx token");
    static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if(initialized != CURLE_OK)
        return absl::UnavailableError("curl initialization failed");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if(!curl)
        return absl::UnavailableError("curl handle initialization failed");
    auto escaped = [&](const std::string& value)
    {
        char* raw = curl_easy_escape(curl.get(), value.c_str(), static_cast<int>(value.size()));
        if(!raw)
            throw std::bad_alloc();
        std::string result(raw);
        curl_free(raw);
        return result;
    };
    std::string url;
    try
    {
        url = options_.url + "/api/v2/write?org=" + escaped(options_.org) + "&bucket=" + escaped(options_.bucket) +
              "&precision=ns";
    }
    catch(...)
    {
        return absl::ResourceExhaustedError("URL encoding failed");
    }
    curl_slist* raw = nullptr;
    raw = curl_slist_append(raw, ("Authorization: Token " + options_.token).c_str());
    raw = curl_slist_append(raw, "Content-Type: text/plain; charset=utf-8");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    auto delay = std::min(options_.backoff, options_.max_backoff);
    for(size_t attempt = 0; attempt <= options_.retries; ++attempt)
    {
        auto remaining = deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                            *deadline - std::chrono::system_clock::now())
                                  : options_.timeout;
        if(remaining.count() <= 0)
            return absl::DeadlineExceededError("Influx deadline");
        curl_easy_setopt(curl.get(),
                         CURLOPT_TIMEOUT_MS,
                         static_cast<long>(std::min(remaining, options_.timeout).count()));
        auto code = curl_easy_perform(curl.get());
        long http{};
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http);
        if(code == CURLE_OK && http == 204)
            return absl::OkStatus();
        bool retry = code == CURLE_OK ? (http == 429 || (http >= 500 && http <= 599)) : retryable(code);
        if(!retry)
            return http == 401 || http == 403
                           ? absl::PermissionDeniedError("Influx HTTP " + std::to_string(http))
                           : absl::FailedPreconditionError("Influx rejected write: HTTP " + std::to_string(http) +
                                                           " curl " + std::to_string(code));
        if(attempt == options_.retries)
            return absl::UnavailableError("Influx retries exhausted: HTTP " + std::to_string(http));
        if(deadline && std::chrono::system_clock::now() + delay >= *deadline)
            return absl::DeadlineExceededError("Influx retry deadline");
        std::this_thread::sleep_for(delay);
        delay = std::min(options_.max_backoff, delay * 2);
    }
    return absl::UnavailableError("Influx unavailable");
}
Batch::Batch(size_t count, std::chrono::milliseconds age, size_t bytes)
    : max_count_(count)
    , max_bytes_(bytes)
    , max_age_(age)
{}
bool Batch::full(size_t extra) const
{
    return count_ >= max_count_ || extra > max_bytes_ - std::min(body_.size(), max_bytes_);
}
bool Batch::due(std::chrono::steady_clock::time_point now) const
{
    return count_ && (count_ >= max_count_ || now - started_ >= max_age_);
}
absl::Status Batch::add(const std::string& story, client::Position position, std::string line)
{
    if(full(line.size()))
        return absl::ResourceExhaustedError("export batch is full");
    if(!count_)
        started_ = std::chrono::steady_clock::now();
    ++count_;
    body_ += line;
    positions_[story] = position;
    return absl::OkStatus();
}
void Batch::clear()
{
    count_ = 0;
    body_.clear();
    positions_.clear();
}
} // namespace chronolog::stream
