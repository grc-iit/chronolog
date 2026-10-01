#include "chronolog/stream/stream.h"
#include <arpa/inet.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace chronolog;
namespace
{
std::string visor, player;
class HttpSink
{
    int socket_;
    std::vector<int> responses_;
    std::mutex mutex_;
    std::vector<std::string> requests_;
    std::jthread worker_;
    void serve(std::stop_token stop)
    {
        while(!stop.stop_requested())
        {
            pollfd ready{socket_, POLLIN, 0};
            if(poll(&ready, 1, 50) <= 0)
                continue;
            int peer = accept(socket_, nullptr, nullptr);
            if(peer < 0)
                continue;
            timeval timeout{1, 0};
            setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::string request;
            size_t end = std::string::npos, length{};
            char buffer[4096];
            while(request.size() < (1u << 20) + 16384)
            {
                auto n = recv(peer, buffer, sizeof(buffer), 0);
                if(n <= 0)
                    break;
                request.append(buffer, static_cast<size_t>(n));
                end = request.find("\r\n\r\n");
                if(end != std::string::npos)
                {
                    auto at = request.find("Content-Length: ");
                    if(at != std::string::npos)
                        length = std::stoull(request.substr(at + 16));
                    if(request.size() >= end + 4 + length)
                        break;
                }
            }
            int status = 204;
            {
                std::lock_guard lock(mutex_);
                if(requests_.size() < 128)
                {
                    size_t index = requests_.size();
                    if(index < responses_.size())
                        status = responses_[index];
                    requests_.push_back(request);
                }
                else
                    status = 503;
            }
            if(status == 204 && end != std::string::npos)
            {
                auto body = request.substr(end + 4, length);
                points.fetch_add(static_cast<size_t>(std::count(body.begin(), body.end(), '\n')));
            }
            auto response =
                    "HTTP/1.1 " + std::to_string(status) + " test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(peer, response.data(), response.size(), MSG_NOSIGNAL);
            close(peer);
        }
    }

public:
    std::atomic<size_t> points{};
    std::string url;
    explicit HttpSink(std::vector<int> responses = {})
        : socket_(socket(AF_INET, SOCK_STREAM, 0))
        , responses_(std::move(responses))
    {
        if(socket_ < 0)
            throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(socket_, 8) != 0)
            throw std::runtime_error("HTTP bind failed");
        socklen_t length = sizeof(address);
        getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        worker_ = std::jthread([this](std::stop_token stop) { serve(stop); });
    }
    ~HttpSink()
    {
        worker_.request_stop();
        worker_.join();
        close(socket_);
    }
    std::vector<std::string> requests()
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }
};
std::string temp()
{
    static std::atomic<size_t> n{};
    auto path = (std::filesystem::temp_directory_path() /
                 ("chronolog-stream-" + std::to_string(getpid()) + "-" + std::to_string(++n)))
                        .string();
    std::filesystem::create_directories(path);
    return path;
}
void write(const std::string& path, const std::string& value) { std::ofstream(path) << value; }
stream::ProcPaths fixtures(const std::string& root)
{
    write(root + "/stat", "cpu 100 0 30 400 10 2 3 5 50 25\ncpu0 1 2 3\n");
    write(root + "/meminfo", "MemTotal: 1024 kB\nMemFree: 64 kB\nMemAvailable: 256 kB\nHugePages_Total: 0\n");
    write(root + "/net",
          "header\nheader\nlo: 3 4 0 0 0 0 0 0 5 6 0 0 0 0 0 0\neth0: 100 2 0 0 0 0 0 0 200 4 0 0 0 0 0 0\n");
    return {root + "/stat", root + "/meminfo", root + "/net"};
}
client::Client connect()
{
    client::ClientOptions o;
    o.catalog_endpoint = visor;
    o.player_endpoint = player;
    auto c = client::Client::Connect(o);
    if(!c.ok())
        throw std::runtime_error(c.status().ToString());
    return std::move(*c);
}
} // namespace
TEST(StreamParsers, CpuMemoryAndInterfaces)
{
    auto root = temp();
    auto paths = fixtures(root);
    auto cpu = stream::parseCpu(paths.cpu);
    ASSERT_TRUE(cpu.ok());
    EXPECT_EQ((*cpu)[0], 100);
    EXPECT_EQ((*cpu)[7], 5);
    auto after = *cpu;
    after[0] += 10;
    after[3] += 10;
    auto utilization = stream::cpuUtilization(*cpu, after);
    EXPECT_DOUBLE_EQ(utilization.values["user"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(utilization.values["total"].get<double>(), 0.5);
    EXPECT_EQ(stream::cpuUtilization(*cpu, *cpu).values["total"], 0.0);
    auto memory = stream::parseMemory(paths.memory);
    ASSERT_TRUE(memory.ok());
    EXPECT_EQ(memory->values["total"], 1048576);
    EXPECT_EQ(memory->values["available"], 262144);
    EXPECT_EQ(memory->values["used"], 786432);
    auto network = stream::parseNetwork(paths.network);
    ASSERT_TRUE(network.ok());
    ASSERT_EQ(network->size(), 1);
    EXPECT_EQ(network->front().labels.at("network.interface.name"), "eth0");
    EXPECT_EQ(network->front().values["tx_packets"], 4);
    EXPECT_EQ(stream::parseNetwork(paths.network, true)->size(), 2);
    write(paths.cpu, "cpu 1 2 -3\n");
    EXPECT_FALSE(stream::parseCpu(paths.cpu).ok());
    write(paths.memory, "MemTotal: 1 kB\nMemAvailable: 2 kB\n");
    EXPECT_FALSE(stream::parseMemory(paths.memory).ok());
    write(paths.network, "header\nheader\neth0: bad\n");
    EXPECT_FALSE(stream::parseNetwork(paths.network).ok());
    EXPECT_FALSE(stream::parseCpu(root + "/absent").ok());
    std::filesystem::remove_all(root);
}
TEST(StreamTransform, EscapesExactValuesAndPhysicalTime)
{
    Event event;
    event.physical.physical_ns = 123;
    event.envelope.content_type = "application/vnd.chronolog.metric+json";
    event.envelope.attributes["host.name"] = "h ost,=\\";
    event.envelope.payload =
            stream::Json({{"name", "m et,ric"},
                          {"values", {{"field =", 1.25}, {"integer", int64_t{-2}}, {"unsigned", uint64_t{UINT64_MAX}}}},
                          {"labels", {{"tag =", "a,b c=\\"}}}})
                    .dump();
    auto line = stream::lineProtocol(event);
    ASSERT_TRUE(line.ok()) << line.status();
    EXPECT_EQ(*line,
              "m\\ et\\,ric,host.name=h\\ ost\\,\\=\\\\,tag\\ \\==a\\,b\\ c\\=\\\\ field\\ "
              "\\==1.25,integer=-2i,unsigned=18446744073709551615u 123\n");
    event.envelope.payload = "{\"name\":\"a\",\"values\":{\"value\":\"bad\"}}";
    EXPECT_FALSE(stream::lineProtocol(event).ok());
    event.envelope.payload = "{\"name\":\"bad\\nmeasurement\",\"value\":1}";
    EXPECT_FALSE(stream::lineProtocol(event).ok());
}
TEST(StreamBatch, CountAgeAndBytes)
{
    stream::Batch batch(2, std::chrono::milliseconds(20), 32);
    client::Position position{{1, 0}, {1, 1, 1, 1}};
    ASSERT_TRUE(batch.add("cpu", position, "a\n").ok());
    EXPECT_FALSE(batch.full(1));
    EXPECT_TRUE(batch.due(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    position.id.sequence = 2;
    ASSERT_TRUE(batch.add("cpu", position, "b\n").ok());
    EXPECT_TRUE(batch.full(1));
    EXPECT_EQ(batch.positions().at("cpu").id.sequence, 2);
    EXPECT_EQ(batch.body(), "a\nb\n");
    batch.clear();
    EXPECT_EQ(batch.count(), 0);
    EXPECT_TRUE(batch.positions().empty());
    EXPECT_TRUE(batch.full(33));
}
TEST(StreamHttp, RetriesTransientFailuresAndRequires204)
{
    HttpSink server({429, 503, 204});
    stream::InfluxSink sink({.url = server.url,
                             .org = "test org",
                             .bucket = "b&x",
                             .token = "test",
                             .backoff = std::chrono::milliseconds(1)});
    ASSERT_TRUE(sink.post("cpu value=1 123\n").ok());
    auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3);
    EXPECT_EQ(requests[0], requests[1]);
    EXPECT_EQ(requests[1], requests[2]);
    EXPECT_NE(requests[0].find("org=test%20org&bucket=b%26x"), std::string::npos);
    for(int status: {400, 401, 403, 404, 200, 201})
    {
        HttpSink rejected({status});
        stream::InfluxSink bad({.url = rejected.url, .backoff = std::chrono::milliseconds(1)});
        EXPECT_FALSE(bad.post("cpu value=1 123\n").ok());
        EXPECT_EQ(rejected.requests().size(), 1);
    }
    std::string closedUrl;
    {
        HttpSink closed;
        closedUrl = closed.url;
    }
    stream::InfluxSink disconnected({.url = closedUrl,
                                     .retries = 1,
                                     .timeout = std::chrono::milliseconds(50),
                                     .backoff = std::chrono::milliseconds(1)});
    EXPECT_TRUE(absl::IsUnavailable(disconnected.post("cpu value=1 123\n")));
    HttpSink exhausted({503, 503});
    stream::InfluxSink retry({.url = exhausted.url, .retries = 1, .backoff = std::chrono::milliseconds(1)});
    EXPECT_TRUE(absl::IsUnavailable(retry.post("cpu value=1 123\n")));
    EXPECT_EQ(exhausted.requests().size(), 2);
    EXPECT_TRUE(absl::IsDeadlineExceeded(
            sink.post("a value=1 1\n", std::chrono::system_clock::now() - std::chrono::seconds(1))));
}
TEST(StreamStack, CollectorExporterResumesAndSavesOnlyAcknowledgedBatches)
{
    if(visor.empty())
        GTEST_SKIP();
    auto root = temp();
    auto paths = fixtures(root);
    auto client = connect();
    stream::Collector collector(client, "stream-test", "fixture-host", paths);
    Envelope metadata;
    metadata.trace_id = std::string(16, 't');
    metadata.span_id = std::string(8, 's');
    for(int i = 0; i < 2; ++i)
    {
        auto samples = collector.sample(Durability::Durable, metadata);
        ASSERT_TRUE(samples.ok()) << samples.status();
        ASSERT_EQ(samples->size(), 3);
        for(const auto& r: *samples)
        {
            ASSERT_TRUE(r.ok());
            EXPECT_TRUE(r->acked());
        }
    }
    auto id = stream::findStory(client, "stream-test", "system.cpu", false);
    ASSERT_TRUE(id.ok());
    auto reader = client.read(*id, {{}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(reader.ok());
    auto item = reader->next(std::chrono::system_clock::now() + std::chrono::seconds(2));
    ASSERT_TRUE(item.ok());
    ASSERT_TRUE(*item);
    ASSERT_FALSE((**item).events.empty());
    EXPECT_EQ((**item).events[0].envelope.trace_id, std::string(16, 't'));
    EXPECT_EQ((**item).events[0].envelope.attributes.at("host.name"), "fixture-host");
    reader->cancel();
    HttpSink rejected({401});
    stream::InfluxSink fail({.url = rejected.url});
    kvs::Store cursors(client, "stream-test.cursors");
    stream::Exporter denied(client, cursors, "stream-test", {"system.cpu"}, fail, {.batch_count = 1});
    auto failure = denied.run([] { return false; }, std::chrono::system_clock::now() + std::chrono::seconds(2));
    EXPECT_TRUE(absl::IsPermissionDenied(failure.status()));
    auto absent = denied.saved("system.cpu");
    ASSERT_TRUE(absent.ok());
    EXPECT_FALSE(*absent);
    HttpSink server({503, 429, 204});
    stream::InfluxSink sink({.url = server.url, .backoff = std::chrono::milliseconds(1)});
    stream::Exporter exporter(client,
                              cursors,
                              "stream-test",
                              {"system.cpu", "system.memory", "system.network"},
                              sink,
                              {.batch_count = 2, .batch_age = std::chrono::milliseconds(30)});
    auto exported = exporter.run([&] { return server.points.load() >= 6; },
                                 std::chrono::system_clock::now() + std::chrono::seconds(5));
    ASSERT_TRUE(exported.ok()) << exported.status();
    EXPECT_EQ(exported->events, 6);
    EXPECT_EQ(server.points.load(), 6);
    for(const auto& story: {"system.cpu", "system.memory", "system.network"})
    {
        auto saved = exporter.saved(story);
        ASSERT_TRUE(saved.ok());
        ASSERT_TRUE(*saved);
        EXPECT_EQ((**saved).id.sequence, 1);
    }
    auto sample = collector.sample();
    ASSERT_TRUE(sample.ok());
    auto restarted = connect();
    kvs::Store loaded(restarted, "stream-test.cursors");
    stream::Exporter resumed(restarted,
                             loaded,
                             "stream-test",
                             {"system.cpu", "system.memory", "system.network"},
                             sink,
                             {.batch_count = 100, .batch_age = std::chrono::milliseconds(1000)});
    auto count = server.points.load();
    auto result = resumed.run([] { return false; }, std::chrono::system_clock::now() + std::chrono::milliseconds(300));
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->events, 3);
    EXPECT_EQ(server.points.load() - count, 3);
    auto requests = server.requests();
    EXPECT_EQ(requests[0], requests[1]);
    EXPECT_EQ(requests[1], requests[2]);
    std::filesystem::remove_all(root);
}
int main(int argc, char** argv)
{
    if(argc == 3 && argv[1][0] != '-')
    {
        visor = argv[1];
        player = argv[2];
        argc = 1;
        ::testing::GTEST_FLAG(filter) = "StreamStack.*";
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
