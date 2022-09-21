//
// Created by kfeng on 7/18/22.
//
#include <cassert>
#include <atomic>
#include "client.h"
#include "log.h"
#include "common.h"

#define NUM_THREAD (1)

int main() {
    ChronoLogClient client;
    std::string server_uri = "ofi+sockets://127.0.0.1:6666";
    std::vector<std::string> client_ids;
    int flags = 0;
    bool ret = false;
    std::chrono::steady_clock::time_point t1, t2;
    std::atomic<double> duration_connect{}, duration_disconnect{};

    client_ids.reserve(NUM_THREAD);
    for (int i = 0; i < NUM_THREAD; i++) {
        t1 = std::chrono::steady_clock::now();
        ret = client.Connect(server_uri, client_ids[i]);
        assert(ret != false);
        t2 = std::chrono::steady_clock::now();
        double duration = std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count();
        std::atomic_fetch_add(&duration_connect, duration);
    }

    for (int i = 0; i < NUM_THREAD; i++) {
        t1 = std::chrono::steady_clock::now();
        ret = client.Disconnect(client_ids[i], flags);
        assert(ret != false);
        t2 = std::chrono::steady_clock::now();
        duration_disconnect += (t2 - t1).count();
    };

    LOGI("CreateChronicle takes %lf ns", duration_connect / NUM_THREAD);
    LOGI("AcquireChronicle takes %lf ns", duration_disconnect / NUM_THREAD);

    return 0;
}