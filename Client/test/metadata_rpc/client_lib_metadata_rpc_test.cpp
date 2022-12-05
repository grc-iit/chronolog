//
// Created by kfeng on 5/17/22.
//

#include <client.h>
#include <global_var_client.h>
#include <log.h>
#include <common.h>

#define NUM_CHRONICLE (1)
#define NUM_STORY (1)

#define CHRONICLE_NAME_LEN 32
#define STORY_NAME_LEN 32

int main() {
    ChronoLogClient client("../../../../test/communication/server_list");
    std::vector<std::string> chronicle_names;
    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration<double, std::nano> duration_create_chronicle{},
            duration_acquire_chronicle{},
            duration_edit_chronicle_attr{},
            duration_create_story{},
            duration_acquire_story{},
            duration_release_story{},
            duration_destroy_story{},
            duration_get_chronicle_attr{},
            duration_release_chronicle{},
            duration_destroy_chronicle{};
    
    chronicle_names.reserve(NUM_CHRONICLE);
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        std::string chronicle_name(gen_random(CHRONICLE_NAME_LEN));
        chronicle_names.emplace_back(chronicle_name);
        std::string attr = std::string("Priority=High");
        bool ret = false;
        std::unordered_map<std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_name, chronicle_attrs);
        t2 = std::chrono::steady_clock::now();
        LOGD("ret: %d", ret);
        duration_create_chronicle += (t2 - t1);

        t1 = std::chrono::steady_clock::now();
        ret = client.AcquireChronicle(chronicle_name, 1);
        t2 = std::chrono::steady_clock::now();
        LOGD("ret: %d", ret);
        duration_acquire_chronicle += (t2 - t1);

        std::string key("Date");
        t1 = std::chrono::steady_clock::now();
        ret = client.EditChronicleAttr(chronicle_name, key, "2022-04-20");
        t2 = std::chrono::steady_clock::now();
        LOGD("ret: %d", ret);
        duration_edit_chronicle_attr += (t2 - t1);

        std::vector<std::string> story_names;
        story_names.reserve(NUM_STORY);
        for (int j = 0; j < NUM_STORY; j++) {
            std::string story_name(gen_random(STORY_NAME_LEN));
            story_names.emplace_back(story_name);
            std::unordered_map<std::string, std::string> story_attrs;
            chronicle_attrs.emplace("Priority", "High");
            chronicle_attrs.emplace("IndexGranularity", "Millisecond");
            chronicle_attrs.emplace("TieringPolicy", "Hot");
            t1 = std::chrono::steady_clock::now();
            ret = client.CreateStory(chronicle_name, story_name, story_attrs);
            t2 = std::chrono::steady_clock::now();
            LOGD("ret: %d", ret);
            duration_create_story += (t2 - t1);

            t1 = std::chrono::steady_clock::now();
            ret = client.AcquireStory(chronicle_name, story_name, 2);
            t2 = std::chrono::steady_clock::now();
            LOGD("ret: %d", ret);
            duration_acquire_story += (t2 - t1);

            t1 = std::chrono::steady_clock::now();
            ret = client.ReleaseStory(chronicle_name, story_name, 4);
            t2 = std::chrono::steady_clock::now();
            LOGD("ret: %d", ret);
            duration_release_story += (t2 - t1);
        }

        for (int j = 0; j < NUM_STORY; j++) {
            t1 = std::chrono::steady_clock::now();
            ret = client.DestroyStory(chronicle_name, story_names[j], 8);
            t2 = std::chrono::steady_clock::now();
            LOGD("ret: %d", ret);
            duration_destroy_story += (t2 - t1);
        }

        t1 = std::chrono::steady_clock::now();
        std::string value = client.GetChronicleAttr(chronicle_name, key);
        t2 = std::chrono::steady_clock::now();
        LOGD("value: %s", value.c_str());
        duration_get_chronicle_attr += (t2 - t1);

        t1 = std::chrono::steady_clock::now();
        ret = client.ReleaseChronicle(chronicle_name, 16);
        t2 = std::chrono::steady_clock::now();
        LOGD("ret: %d", ret);
        duration_release_chronicle += (t2 - t1);
    }

    for (int i = 0; i < NUM_CHRONICLE; i++) {
        t1 = std::chrono::steady_clock::now();
        bool ret = client.DestroyChronicle(chronicle_names[i], 32);
        t2 = std::chrono::steady_clock::now();
        LOGD("ret: %d", ret);
        duration_destroy_chronicle += (t2 - t1);
    };

    LOGI("CreateChronicle takes %lf ns", duration_create_chronicle.count() / NUM_CHRONICLE);
    LOGI("AcquireChronicle takes %lf ns", duration_acquire_chronicle.count() / NUM_CHRONICLE);
    LOGI("EditChronicleAttr takes %lf ns", duration_edit_chronicle_attr.count() / NUM_CHRONICLE);
    LOGI("CreateStory takes %lf ns", duration_create_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOGI("AcquireStory takes %lf ns", duration_acquire_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOGI("ReleaseStory takes %lf ns", duration_release_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOGI("DestroyStory takes %lf ns", duration_destroy_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOGI("GetChronileAttr(Date) takes %lf ns", duration_get_chronicle_attr.count() / NUM_CHRONICLE);
    LOGI("ReleaseChronicle takes %lf ns", duration_release_chronicle.count() / NUM_CHRONICLE);
    LOGI("DestroyChronicle takes %lf ns", duration_destroy_chronicle.count() / NUM_CHRONICLE);

    duration_create_chronicle = std::chrono::duration<double, std::nano>();
    chronicle_names.clear();
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        std::string chronicle_name(gen_random(CHRONICLE_NAME_LEN));
        chronicle_names.emplace_back(chronicle_name);
        std::string attr = std::string("Priority=High");
        bool ret = false;
        std::unordered_map<std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_name, chronicle_attrs);
        t2 = std::chrono::steady_clock::now();
        LOGD("ret: %d", ret);
        duration_create_chronicle += (t2 - t1);
    }

    duration_destroy_chronicle = std::chrono::duration<double, std::nano>();
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        t1 = std::chrono::steady_clock::now();
        bool ret = client.DestroyChronicle(chronicle_names[i], 32);
        t2 = std::chrono::steady_clock::now();
        LOGD("ret: %d", ret);
        duration_destroy_chronicle += (t2 - t1);
    }

    LOGI("CreateChronicle2 takes %lf ns", duration_create_chronicle.count() / NUM_CHRONICLE);
    LOGI("DestroyChronicle2 takes %lf ns", duration_destroy_chronicle.count() / NUM_CHRONICLE);

    return 0;
}