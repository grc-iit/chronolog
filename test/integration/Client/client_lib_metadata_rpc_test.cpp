//
// Created by kfeng on 5/17/22.
//

#include "client.h"
#include "common.h"
#include <cassert>

#define NUM_CHRONICLE (10)
#define NUM_STORY (10)

#define CHRONICLE_NAME_LEN 32
#define STORY_NAME_LEN 32

int main() {
    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    std::string server_ip = "127.0.0.1";
    int base_port = 5555;
    ChronoLogClient client(protocol, server_ip, base_port);
    std::vector<std::string> chronicle_names;
    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration<double, std::nano> duration_create_chronicle{},
            duration_edit_chronicle_attr{},
            duration_acquire_story{},
            duration_release_story{},
            duration_destroy_story{},
            duration_get_chronicle_attr{},
            duration_destroy_chronicle{},
            duration_show_chronicles{},
            duration_show_stories{};
    int flags;
    int ret;

    chronicle_names.reserve(NUM_CHRONICLE);
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        std::string chronicle_name(gen_random(CHRONICLE_NAME_LEN));
        chronicle_names.emplace_back(chronicle_name);
    }

    for (int i = 0; i < NUM_CHRONICLE; i++) {
        std::string attr = std::string("Priority=High");
        std::unordered_map<std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_names[i], chronicle_attrs, flags);
        t2 = std::chrono::steady_clock::now();
        ASSERT(ret, ==, CL_SUCCESS);
        duration_create_chronicle += (t2 - t1);
    }

    std::string client_id;
    t1 = std::chrono::steady_clock::now();
    std::vector<std::string> chronicle_names_retrieved = client.ShowChronicles(client_id);
    t2 = std::chrono::steady_clock::now();
    duration_show_chronicles += (t2 - t1);
    std::sort(chronicle_names_retrieved.begin(), chronicle_names_retrieved.end());
    std::vector<std::string> chronicle_names_sorted = chronicle_names;
    std::sort(chronicle_names_sorted.begin(), chronicle_names_sorted.end());
    assert(chronicle_names_retrieved == chronicle_names_sorted);

    for (int i = 0; i < NUM_CHRONICLE; i++) {
        std::string key("Date");
        t1 = std::chrono::steady_clock::now();
        ret = client.EditChronicleAttr(chronicle_names[i], key, "2023-01-15");
        t2 = std::chrono::steady_clock::now();
        //ASSERT(ret, ==, CL_SUCCESS);
        duration_edit_chronicle_attr += (t2 - t1);

        std::vector<std::string> story_names;
        story_names.reserve(NUM_STORY);
        for (int j = 0; j < NUM_STORY; j++) {
            flags = 2;
            std::string story_name(gen_random(STORY_NAME_LEN));
            story_names.emplace_back(story_name);
            std::unordered_map<std::string, std::string> story_attrs;
            story_attrs.emplace("Priority", "High");
            story_attrs.emplace("IndexGranularity", "Millisecond");
            story_attrs.emplace("TieringPolicy", "Hot");
            t1 = std::chrono::steady_clock::now();
            ret = client.AcquireStory(chronicle_names[i], story_names[j], story_attrs, flags);
            t2 = std::chrono::steady_clock::now();
            ASSERT(ret, ==, CL_SUCCESS);
            duration_acquire_story += (t2 - t1);
        }

        t1 = std::chrono::steady_clock::now();
        std::vector<std::string> stories_names_retrieved = client.ShowStories(client_id, chronicle_names[i]);
        t2 = std::chrono::steady_clock::now();
        duration_show_stories += (t2 - t1);
        std::sort(stories_names_retrieved.begin(), stories_names_retrieved.end());
        std::vector<std::string> story_names_sorted = story_names;
        std::sort(story_names_sorted.begin(), story_names_sorted.end());
        assert(stories_names_retrieved == story_names_sorted);

        for (int j = 0; j < NUM_STORY; j++) {
            flags = 4;
            t1 = std::chrono::steady_clock::now();
            ret = client.ReleaseStory(chronicle_names[i], story_names[j], flags);
            t2 = std::chrono::steady_clock::now();
            ASSERT(ret, ==, CL_SUCCESS);
            duration_release_story += (t2 - t1);
        }

        flags = 8;
        for (int j = 0; j < NUM_STORY; j++) {
            t1 = std::chrono::steady_clock::now();
            ret = client.DestroyStory(chronicle_names[i], story_names[j], flags);
            t2 = std::chrono::steady_clock::now();
            ASSERT(ret, ==, CL_SUCCESS);
            duration_destroy_story += (t2 - t1);
        }

        std::string value = "pls_ignore";
        t1 = std::chrono::steady_clock::now();
        ret = client.GetChronicleAttr(chronicle_names[i], key, value);
        t2 = std::chrono::steady_clock::now();
        //ASSERT(ret, ==, CL_SUCCESS);
        //FIXME: returning data using parameter is not working, the following assert will fail
        //ASSERT(value, ==, "2023-01-15");
        duration_get_chronicle_attr += (t2 - t1);
    }

    flags = 32;
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        t1 = std::chrono::steady_clock::now();
        bool ret = client.DestroyChronicle(chronicle_names[i], flags);
        t2 = std::chrono::steady_clock::now();
        ASSERT(ret, ==, CL_SUCCESS);
        duration_destroy_chronicle += (t2 - t1);
    };

    LOGI("CreateChronicle takes %lf ns", duration_create_chronicle.count() / NUM_CHRONICLE);
    LOGI("EditChronicleAttr takes %lf ns", duration_edit_chronicle_attr.count() / NUM_CHRONICLE);
    LOGI("AcquireStory takes %lf ns", duration_acquire_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOGI("ReleaseStory takes %lf ns", duration_release_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOGI("DestroyStory takes %lf ns", duration_destroy_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOGI("GetChronileAttr(Date) takes %lf ns", duration_get_chronicle_attr.count() / NUM_CHRONICLE);
    LOGI("DestroyChronicle takes %lf ns", duration_destroy_chronicle.count() / NUM_CHRONICLE);
    LOGI("ShowChronicles takes %lf ns", duration_show_chronicles.count() / NUM_CHRONICLE);
    LOGI("ShowStories takes %lf ns", duration_show_stories.count() / NUM_CHRONICLE);

    duration_create_chronicle = std::chrono::duration<double, std::nano>();
    chronicle_names.clear();
    flags = 1;
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        std::string chronicle_name(gen_random(CHRONICLE_NAME_LEN));
        chronicle_names.emplace_back(chronicle_name);
        std::string attr = std::string("Priority=High");
        int ret;
        std::unordered_map<std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
        t2 = std::chrono::steady_clock::now();
        ASSERT(ret, ==, CL_SUCCESS);
        duration_create_chronicle += (t2 - t1);
    }

    flags = 32;
    duration_destroy_chronicle = std::chrono::duration<double, std::nano>();
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        t1 = std::chrono::steady_clock::now();
        int ret = client.DestroyChronicle(chronicle_names[i], flags);
        t2 = std::chrono::steady_clock::now();
        ASSERT(ret, ==, CL_SUCCESS);
        duration_destroy_chronicle += (t2 - t1);
    }

    LOGI("CreateChronicle2 takes %lf ns", duration_create_chronicle.count() / NUM_CHRONICLE);
    LOGI("DestroyChronicle2 takes %lf ns", duration_destroy_chronicle.count() / NUM_CHRONICLE);

    return 0;
}
