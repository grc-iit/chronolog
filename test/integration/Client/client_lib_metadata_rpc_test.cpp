//
// Created by kfeng on 5/17/22.
//

#include <chrono>
#include "chronolog_client.h"
#include "common.h"
#include <cassert>
#include <cmd_arg_parse.h>
#include "chrono_monitor.h"

#define NUM_CHRONICLE (10)
#define NUM_STORY (10)

#define CHRONICLE_NAME_LEN 32
#define STORY_NAME_LEN 32

int main(int argc, char**argv)
{
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if(conf_file_path.empty())
    {
        std::exit(EXIT_FAILURE);
    }
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = chronolog::chrono_monitor::initialize(confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("[ClientLibMetadataRPCTest] Running test.");

    chronolog::Client client(confManager);
    std::vector <std::string> chronicle_names;
    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration <double, std::nano> duration_create_chronicle{}, duration_edit_chronicle_attr{}, duration_acquire_story{}, duration_release_story{}, duration_destroy_story{}, duration_get_chronicle_attr{}, duration_destroy_chronicle{}, duration_show_chronicles{}, duration_show_stories{};
    int flags;
    int ret;
    uint64_t offset = 0;
    std::string server_uri = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
                             confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP + std::to_string(
            confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT);

    std::string client_id = gen_random(8);
    LOG_INFO("[ClientLibMetadataRPCTest] Connecting to server with URI: {}", server_uri);
    client.Connect();
    chronicle_names.reserve(NUM_CHRONICLE);
    LOG_INFO("[ClientLibMetadataRPCTest] Starting creation of {} chronicles.", NUM_CHRONICLE);
    for(int i = 0; i < NUM_CHRONICLE; i++)
    {
        std::string chronicle_name(gen_random(CHRONICLE_NAME_LEN));
        chronicle_names.emplace_back(chronicle_name);
        LOG_INFO("[ClientLibMetadataRPCTest] Created chronicle: {}", chronicle_names[i]);
    }

    for(int i = 0; i < NUM_CHRONICLE; i++)
    {
        std::string attr = std::string("Priority=High");
        std::map <std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("Date", "2023-01-15");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_names[i], chronicle_attrs, flags);
        t2 = std::chrono::steady_clock::now();
        assert(ret == chronolog::CL_SUCCESS);
        duration_create_chronicle += (t2 - t1);
    }


    t1 = std::chrono::steady_clock::now();
    std::vector <std::string> chronicle_names_retrieved;
    chronicle_names_retrieved = client.ShowChronicles(chronicle_names_retrieved);
    t2 = std::chrono::steady_clock::now();
    duration_show_chronicles += (t2 - t1);
    //std::sort(chronicle_names_retrieved.begin(), chronicle_names_retrieved.end());
    std::vector <std::string> chronicle_names_sorted = chronicle_names;
    //std::sort(chronicle_names_sorted.begin(), chronicle_names_sorted.end());
    //assert(chronicle_names_retrieved == chronicle_names_sorted);


    for(int i = 0; i < NUM_CHRONICLE; i++)
    {
        std::string key("Date");
        t1 = std::chrono::steady_clock::now();
        ret = client.EditChronicleAttr(chronicle_names[i], key, "2023-01-15");
        t2 = std::chrono::steady_clock::now();
        //FIXME:  is not working, the following assert will fail
        //assert(ret == CL_SUCCESS || ret == CL_ERR_NO_KEEPERS);
        duration_edit_chronicle_attr += (t2 - t1);

        std::vector <std::string> story_names;
        story_names.reserve(NUM_STORY);
        for(int j = 0; j < NUM_STORY; j++)
        {
            flags = 2;
            std::string story_name(gen_random(STORY_NAME_LEN));
            story_names.emplace_back(story_name);
            std::map <std::string, std::string> story_attrs;
            story_attrs.emplace("Priority", "High");
            story_attrs.emplace("IndexGranularity", "Millisecond");
            story_attrs.emplace("TieringPolicy", "Hot");
            t1 = std::chrono::steady_clock::now();
            ret = client.AcquireStory(chronicle_names[i], story_names[j], story_attrs, flags).first;
            t2 = std::chrono::steady_clock::now();
            assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NO_KEEPERS);
            duration_acquire_story += (t2 - t1);
        }

        ret = client.Disconnect(); //client_id, flags);
        assert(ret == chronolog::CL_ERR_NO_KEEPERS || ret == chronolog::CL_ERR_ACQUIRED);

        t1 = std::chrono::steady_clock::now();
        std::vector <std::string> stories_names_retrieved;
        client.ShowStories(chronicle_names[i], stories_names_retrieved);
        t2 = std::chrono::steady_clock::now();
        duration_show_stories += (t2 - t1);
        //std::sort(stories_names_retrieved.begin(), stories_names_retrieved.end());
        //std::vector<std::string> story_names_sorted = story_names;
        //std::sort(story_names_sorted.begin(), story_names_sorted.end());
        //assert(stories_names_retrieved == story_names_sorted);

        for(int j = 0; j < NUM_STORY; j++)
        {
            flags = 4;
            t1 = std::chrono::steady_clock::now();
            ret = client.ReleaseStory(chronicle_names[i], story_names[j]); //, flags);
            t2 = std::chrono::steady_clock::now();
            assert(ret == chronolog::CL_SUCCESS);
            duration_release_story += (t2 - t1);
        }

        flags = 8;
        for(int j = 0; j < NUM_STORY; j++)
        {
            t1 = std::chrono::steady_clock::now();
            ret = client.DestroyStory(chronicle_names[i], story_names[j]); // flags);
            t2 = std::chrono::steady_clock::now();
            assert(ret == chronolog::CL_SUCCESS);
            duration_destroy_story += (t2 - t1);
        }

        std::string value = "pls_ignore";
        t1 = std::chrono::steady_clock::now();
        ret = client.GetChronicleAttr(chronicle_names[i], key, value);
        t2 = std::chrono::steady_clock::now();
        //FIXME:assert(ret == CL_SUCCESS);
        //FIXME: returning data using parameter is not working, the following assert will fail
        //ASSERT(value, ==, "2023-01-15");
        duration_get_chronicle_attr += (t2 - t1);
    }

    flags = 32;
    for(int i = 0; i < NUM_CHRONICLE; i++)
    {
        t1 = std::chrono::steady_clock::now();
        bool ret = client.DestroyChronicle(chronicle_names[i]); // flags);
        t2 = std::chrono::steady_clock::now();
        assert(ret == chronolog::CL_SUCCESS);
        duration_destroy_chronicle += (t2 - t1);
    };

    for(int i = 0; i < NUM_STORY; i++)
    {
        std::map <std::string, std::string> story_attrs;
        std::string temp_str = gen_random(STORY_NAME_LEN);
        ret = client.AcquireStory(chronicle_names[i].append(temp_str), temp_str, story_attrs, flags).first;
        assert(ret == chronolog::CL_ERR_NOT_EXIST);
    }

    LOG_INFO("[ClientLibMetadataRPCTest] CreateChronicle takes {} ns", duration_create_chronicle.count() / NUM_CHRONICLE);
    LOG_INFO("[ClientLibMetadataRPCTest] EditChronicleAttr takes {} ns",
            duration_edit_chronicle_attr.count() / NUM_CHRONICLE);
    LOG_INFO("[ClientLibMetadataRPCTest] AcquireStory takes {} ns",
            duration_acquire_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOG_INFO("[ClientLibMetadataRPCTest] ReleaseStory takes {} ns",
            duration_release_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOG_INFO("[ClientLibMetadataRPCTest] DestroyStory takes {} ns",
            duration_destroy_story.count() / (NUM_CHRONICLE * NUM_STORY));
    LOG_INFO("[ClientLibMetadataRPCTest] GetChronileAttr(Date) takes {} ns",
            duration_get_chronicle_attr.count() / NUM_CHRONICLE);
    LOG_INFO("[ClientLibMetadataRPCTest] DestroyChronicle takes {} ns", duration_destroy_chronicle.count() / NUM_CHRONICLE);
    LOG_INFO("[ClientLibMetadataRPCTest] ShowChronicles takes {} ns", duration_show_chronicles.count() / NUM_CHRONICLE);
    LOG_INFO("[ClientLibMetadataRPCTest] ShowStories takes {} ns", duration_show_stories.count() / NUM_CHRONICLE);

    duration_create_chronicle = std::chrono::duration <double, std::nano>();
    chronicle_names.clear();
    flags = 1;
    for(int i = 0; i < NUM_CHRONICLE; i++)
    {
        std::string chronicle_name(gen_random(CHRONICLE_NAME_LEN));
        chronicle_names.emplace_back(chronicle_name);
        std::string attr = std::string("Priority=High");
        int ret;
        std::map <std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        t1 = std::chrono::steady_clock::now();
        ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
        t2 = std::chrono::steady_clock::now();
        assert(ret == chronolog::CL_SUCCESS);
        duration_create_chronicle += (t2 - t1);
    }

    flags = 32;
    duration_destroy_chronicle = std::chrono::duration <double, std::nano>();
    for(int i = 0; i < NUM_CHRONICLE; i++)
    {
        t1 = std::chrono::steady_clock::now();
        int ret = client.DestroyChronicle(chronicle_names[i]);//, flags);
        t2 = std::chrono::steady_clock::now();
        assert(ret == chronolog::CL_SUCCESS);
        duration_destroy_chronicle += (t2 - t1);
    }
    LOG_INFO("[ClientLibMetadataRPCTest] Disconnecting from the server.");
    client.Disconnect();

    LOG_INFO("[ClientLibMetadataRPCTest] CreateChronicle2 takes {} ns", duration_create_chronicle.count() / NUM_CHRONICLE);
    LOG_INFO("[ClientLibMetadataRPCTest] DestroyChronicle2 takes {} ns",
            duration_destroy_chronicle.count() / NUM_CHRONICLE);
    return 0;
}