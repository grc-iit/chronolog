//
// Created by kfeng on 7/18/22.
//
#include <client.h>
#include <global_var_client.h>
#include <common.h>

#define NUM_CONNECTION (1)
#define NUM_THREAD (1)

int main() {
    std::string server_uri = "ofi+sockets://127.0.0.1:5555";
    std::vector<std::string> client_ids;
    std::atomic<long> duration_connect{}, duration_disconnect{};
    std::vector<std::thread> thread_vec;
    uint64_t offset;

    client_ids.reserve(NUM_THREAD);
    thread_vec.reserve(NUM_THREAD);
    for (int i = 0; i < NUM_THREAD; i++) {
        thread_vec.emplace_back([&server_uri,
                                 &i,
                                 &client_ids,
                                 &duration_connect,
                                 &duration_disconnect,
                                 &offset]()
        {
            ChronoLogClient client("../../test/communication/server_list");
            std::chrono::steady_clock::time_point t1, t2;
            bool ret = false;
            int flags = 0;

            t1 = std::chrono::steady_clock::now();
            ret = client.Connect(server_uri, client_ids[i], flags, offset);
            assert(ret != false);
            t2 = std::chrono::steady_clock::now();
            std::atomic_fetch_add(&duration_connect,
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());

            t1 = std::chrono::steady_clock::now();
            ret = client.Disconnect(client_ids[i], flags);
            assert(ret != false);
            t2 = std::chrono::steady_clock::now();
            std::atomic_fetch_add(&duration_disconnect,
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());

            LOGI("CreateChronicle takes %lf ns", (double) duration_connect / NUM_THREAD);
            LOGI("AcquireChronicle takes %lf ns", (double) duration_disconnect / NUM_THREAD);
        });

    };

    return 0;
}