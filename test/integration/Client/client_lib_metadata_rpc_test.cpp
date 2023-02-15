//
// Created by kfeng on 5/17/22.
//

#include "client.h"
#include "common.h"
#include <cassert>

#define NUM_CHRONICLE (1)
#define NUM_STORY (1)

#define CHRONICLE_NAME_LEN 32
#define STORY_NAME_LEN 32

int main() {
    ChronoLog::ConfigurationManager confManager("./default_conf.json");
    ChronoLogClient client(confManager);
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
    int flags;
    uint64_t offset = 0;
    std::string server_uri = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string() + "://" +
                             CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string() +
                             std::to_string(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT);

    std::string client_id = gen_random(8);
    std::string group_id = "metadata_application";
    std::string server_uri = CHRONOLOG_CONF->SOCKETS_CONF.string();
        
   server_uri += "://" + server_ip + ":" + std::to_string(base_port);
   
   uint32_t user_role = CHRONOLOG_CLIENT_RWCD;
   uint32_t group_role = CHRONOLOG_CLIENT_GROUP_REG;
   uint32_t cluster_role = CHRONOLOG_CLIENT_CLUS_REG;
   uint32_t role = role | user_role;
   role = role | (group_role << 3);
   role = role | (cluster_role << 6);   
   uint64_t offset = 0;
   int ret = client.Connect(server_uri, client_id, group_id, role, flags, offset);

    
    chronicle_names.reserve(NUM_CHRONICLE);
    for (int i = 0; i < NUM_CHRONICLE; i++) 
    {
        std::string chronicle_name(gen_random(CHRONICLE_NAME_LEN));
        chronicle_names.emplace_back(chronicle_name);
        std::string attr = std::string("Priority=High");
        int ret;
        std::unordered_map<std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
	chronicle_attrs.emplace("Permissions","RWCD");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
        t2 = std::chrono::steady_clock::now();
        ASSERT(ret, ==, CL_SUCCESS);
        duration_create_chronicle += (t2 - t1);
        
        flags = CHRONOLOG_WRITE;
        t1 = std::chrono::steady_clock::now();
        ret = client.AcquireChronicle(chronicle_name, flags);
        t2 = std::chrono::steady_clock::now();
        ASSERT(ret, ==, CL_SUCCESS);
        duration_acquire_chronicle += (t2 - t1);

        std::string key("Date");
        t1 = std::chrono::steady_clock::now();
        ret = client.EditChronicleAttr(chronicle_name, key, "2023-01-15");
        t2 = std::chrono::steady_clock::now();
        //ASSERT(ret, ==, CL_SUCCESS);
        duration_edit_chronicle_attr += (t2 - t1);

        std::vector<std::string> story_names;
        story_names.reserve(NUM_STORY);
        for (int j = 0; j < NUM_STORY; j++) {
            std::string story_name(gen_random(STORY_NAME_LEN));
            story_names.emplace_back(story_name);
            std::unordered_map<std::string, std::string> story_attrs;
            story_attrs.emplace("Priority", "High");
            story_attrs.emplace("IndexGranularity", "Millisecond");
            story_attrs.emplace("TieringPolicy", "Hot");
	    story_attrs.emplace("Permissions","RWD");
            flags = 1;
            t1 = std::chrono::steady_clock::now();
            ret = client.CreateStory(chronicle_name, story_name, story_attrs, flags);
            t2 = std::chrono::steady_clock::now();
            ASSERT(ret, ==, CL_SUCCESS);
            duration_create_story += (t2 - t1);

            flags = CHRONOLOG_READ;
            t1 = std::chrono::steady_clock::now();
            ret = client.AcquireStory(chronicle_name, story_name, flags);
            t2 = std::chrono::steady_clock::now();
            ASSERT(ret, ==, CL_SUCCESS);
            duration_acquire_story += (t2 - t1);

            flags = 4;
            t1 = std::chrono::steady_clock::now();
            ret = client.ReleaseStory(chronicle_name, story_name, flags);
            t2 = std::chrono::steady_clock::now();
<<<<<<< HEAD
            ASSERT(ret, ==, CL_SUCCESS);
            duration_release_story += (t2 - t1);
=======
            assert(ret == CL_SUCCESS);
            duration_release_story += (t2 - t1); 
>>>>>>> 193dd15 (rebase)
        }
	
        flags = 8;
        for (int j = 0; j < NUM_STORY; j++) {
            t1 = std::chrono::steady_clock::now();
            ret = client.DestroyStory(chronicle_name, story_names[j], flags);
            t2 = std::chrono::steady_clock::now();
            ASSERT(ret, ==, CL_SUCCESS);
            duration_destroy_story += (t2 - t1);
        }

        std::string value = "pls_ignore";
        t1 = std::chrono::steady_clock::now();
        ret = client.GetChronicleAttr(chronicle_name, key, value);
        t2 = std::chrono::steady_clock::now();
        //ASSERT(ret, ==, CL_SUCCESS);
        //FIXME: returning data using parameter is not working, the following assert will fail
        //ASSERT(value, ==, "2023-01-15");
        duration_get_chronicle_attr += (t2 - t1);

        flags = 16;
        t1 = std::chrono::steady_clock::now();
        ret = client.ReleaseChronicle(chronicle_name, flags);
        t2 = std::chrono::steady_clock::now();
        ASSERT(ret, ==, CL_SUCCESS);
        duration_release_chronicle += (t2 - t1);
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
	chronicle_attrs.emplace("Permissions","RWCD");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
        t2 = std::chrono::steady_clock::now();
        assert(ret == CL_SUCCESS);
        duration_create_chronicle += (t2 - t1);
    }

    flags = 32;
    duration_destroy_chronicle = std::chrono::duration<double, std::nano>();
    for (int i = 0; i < NUM_CHRONICLE; i++) {
        t1 = std::chrono::steady_clock::now();
        int ret = client.DestroyChronicle(chronicle_names[i], flags);
        t2 = std::chrono::steady_clock::now();
        assert(ret == CL_SUCCESS);
        duration_destroy_chronicle += (t2 - t1);
    }
    client.Disconnect(client_id,flags);

    LOGI("CreateChronicle2 takes %lf ns", duration_create_chronicle.count() / NUM_CHRONICLE);
    LOGI("DestroyChronicle2 takes %lf ns", duration_destroy_chronicle.count() / NUM_CHRONICLE);

    client.Disconnect(client_id,flags);
    return 0;
}
