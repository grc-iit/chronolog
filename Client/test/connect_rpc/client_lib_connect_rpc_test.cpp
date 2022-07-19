//
// Created by kfeng on 7/11/22.
//

#include <cassert>
#include "client.h"
#include "log.h"
#include "common.h"

#define NUM_CONNECTION (1)

int main() {
    ChronoLogClient client;
    std::string server_uri = "ofi+sockets://127.0.0.1:6666";
    std::vector<std::string> client_ids;
    int flags = 0;
    bool ret = false;
    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration<double, std::nano> duration_connect{},
            duration_disconnect{};

    client_ids.reserve(NUM_CONNECTION);
    for (int i = 0; i < NUM_CONNECTION; i++) {
        t1 = std::chrono::steady_clock::now();
        ret = client.Connect(server_uri, client_ids[i]);
        assert(ret != false);
        t2 = std::chrono::steady_clock::now();
        duration_connect += (t2 - t1);
    }

    for (int i = 0; i < NUM_CONNECTION; i++) {
        t1 = std::chrono::steady_clock::now();
        ret = client.Disconnect(client_ids[i], flags);
        assert(ret != false);
        t2 = std::chrono::steady_clock::now();
        duration_disconnect += (t2 - t1);
    };

    LOGI("CreateChronicle takes %lf ns", duration_connect.count() / NUM_CONNECTION);
    LOGI("AcquireChronicle takes %lf ns", duration_disconnect.count() / NUM_CONNECTION);

    return 0;
}