#include <chronolog_client.h>
#include <cassert>
#include <common.h>
#include <thread>
#include <chrono>
#include <cmd_arg_parse.h>
#include "chrono_monitor.h"

#define STORY_NAME_LEN 5

struct thread_arg
{
    int tid;
    std::string chronicle;
    std::string story;
    uint64_t segment_start;
    uint64_t segment_end;
};

chronolog::Client*client;

void writer_thread(struct thread_arg * t)
{
    LOG_INFO("[ClientLibStoryReader] Writer thread tid={} starting", t->tid);

    // Local variable declarations
    int flags = 1;
    int ret = chronolog::CL_ERR_UNKNOWN;;
    std::map <std::string, std::string> chronicle_attrs;
    std::map <std::string, std::string> story_attrs;

    // Create the chronicle
    ret = client->CreateChronicle(t->chronicle, chronicle_attrs, flags);
    LOG_INFO("[ClientLibStoryReader] Chronicle created: tid={}, ChronicleName={}, Flags: {}", t->tid , t->chronicle, flags);

    // Acquire the story
    auto acquire_ret = client->AcquireStory(t->chronicle, t->story, story_attrs, flags);
    LOG_INFO("[ClientLibStoryReader] Writer thread tid={} acquired story {} {}, Ret: {}", t->tid , t->chronicle, t->story, acquire_ret.first);

    // Assertion for successful story acquisition or expected errors
    assert(acquire_ret.first == chronolog::CL_SUCCESS || acquire_ret.first == chronolog::CL_ERR_NOT_EXIST ||
           acquire_ret.first == chronolog::CL_ERR_NO_KEEPERS);

    // If story acquisition is successful, log events to the story
    if(chronolog::CL_SUCCESS == acquire_ret.first)
    {
        auto story_handle = acquire_ret.second;
        uint64_t timestamp = 0;
        for(int i = 0; i < 10; ++i)
        {
            // Log an event to the story
            timestamp = story_handle->log_event("line " + std::to_string(i));
            if(i== 0)
            { t->segment_start = timestamp; }
            t->segment_end = timestamp;
            std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
        }

        // Release the story
        ret = client->ReleaseStory(t->chronicle, t->story);
        LOG_INFO("[ClientLibStoryReader] Writer thread tid={} released story {} {}, Ret: {}", t->tid , t->chronicle, t->story, ret);

        // Assertion for successful story release or expected errors
        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NO_CONNECTION || ret == chronolog::CL_ERR_NOT_ACQUIRED);
    }

    LOG_INFO("[ClientLibStoryReader] Writer thread tid={} finished logging messages for story {}-{} segment {}-{}", t->tid,t->chronicle,t->story, t->segment_start, t->segment_end);
    LOG_INFO("[ClientLibStoryReader] Writer thread tid={} exiting ", t->tid);
}



void reader_thread( int tid, struct thread_arg * t)
{
    LOG_INFO("[ClientLibStoryReader] Reader thread tid={} starting",tid);

   // t->chronicle="CHRONICLE";
   // t->story="STORY";
   // t->segment_start=1739909340680367449;
   // t->segment_end=1739909342245021766;
    // make the reader thread sleep to allow the writer threads create the story and log some events 
    // allow the events to propagate through the keeper/grappher into ChronoLog store
    while(t->segment_end==0)
    { 
        LOG_INFO("[ClientLibStoryReader] Reader thread tid={} is waiting",tid);
        sleep(60);
    }

    sleep(6*60);

    int ret = chronolog::CL_ERR_UNKNOWN;;
    std::map <std::string, std::string> chronicle_attrs;
    std::map <std::string, std::string> story_attrs;
    int flags = 1;

    // Acquire the story
    auto acquire_ret = client->AcquireStory(t->chronicle, t->story, story_attrs, flags);
    LOG_INFO("[ClientLibStoryReader] Reader thread tid={} acquired story: {} {}, Ret: {}", tid , t->chronicle, t->story, acquire_ret.first);

    // Assertion for successful story acquisition or expected errors
    assert(acquire_ret.first == chronolog::CL_SUCCESS || acquire_ret.first == chronolog::CL_ERR_NOT_EXIST ||
           acquire_ret.first == chronolog::CL_ERR_NO_KEEPERS);

    if(acquire_ret.first == chronolog::CL_SUCCESS)
    {
        auto story_handle = acquire_ret.second;

        LOG_INFO("[ClientLibStoryReader] Reader thread tid={} sending playback_request for story: {} {} segment{}-{}", tid , t->chronicle, t->story, t->segment_start, t->segment_end);

        ret = story_handle->playback_story(t->segment_start, (t->segment_end), playback_events);

        if(ret == chronolog::CL_ERR_NO_PLAYERS)
        {   
            LOG_INFO("[ClientLibStoryReader] Reader thread tid={} can't find Player for story: {} {}, Ret: {}", tid , t->chronicle, t->story, ret);
        }
        else
        {
            LOG_INFO("[ClientLibStoryReader] Reader thread tid={} found Player for story: {} {}, Ret: {}",tid , t->chronicle, t->story, ret);
        }

        // Release the story
        ret = client->ReleaseStory(t->chronicle, t->story);
        LOG_INFO("[ClientLibStoryReader] Reader thread tid={} released story: {} {}, Ret: {}", tid , t->chronicle, t->story, ret);

        // Assertion for successful story release or expected errors
        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NO_CONNECTION);
    }

    LOG_INFO("[ClientLibStoryReader] Reader thread tid={} exiting", tid);
}


int main(int argc, char**argv)
{
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if(conf_file_path.empty())
    {
        std::exit(EXIT_FAILURE);
    }

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
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
    LOG_INFO("[ClientLibStoryReader] Running...");

    int num_threads = 4;

    std::vector <struct thread_arg> t_args(num_threads);
    std::vector <std::thread> workers(num_threads);

    std::string server_ip = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP;
    int base_port = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT;
    client = new chronolog::Client(confManager);

    int ret = client->Connect();

    if(chronolog::CL_SUCCESS != ret)
    {
        LOG_ERROR("[ClientLibStoryReader] Failed to connect to ChronoVisor");
        delete client;
        return -1;
    }

    std::string chronicle_name("CHRONICLE");
    std::string story_name("STORY");

    for(int i = 0; i < num_threads; ++i)
    {
        t_args[i].tid = i;
        t_args[i].chronicle = chronicle_name;
        t_args[i].story = story_name;
        t_args[i].segment_start = 0; 
        t_args[i].segment_end = 0;

        // even threads are writers , odd threads are readers 
        if( i%2 == 0)
        {   workers[i] = std::move( std::thread(writer_thread, &t_args[i])); }
        else
        {   workers[i] = std::move( std::thread(reader_thread, i , &t_args[i-1])); }
        
    }

    for(int i = 0; i < num_threads; i++)
        workers[i].join();

    // Destroy the story
    ret = client->DestroyStory(chronicle_name, story_name);
    LOG_INFO("[ClientLibStoryReader] Story destroyed: {} {}, Ret: {}",  chronicle_name, story_name, ret);

    // Assertion for successful story destruction or expected errors
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED ||
           ret == chronolog::CL_ERR_NO_CONNECTION);

    // Destroy the chronicle
    ret = client->DestroyChronicle(chronicle_name);
    LOG_INFO("[ClientLibStoryReader] Chronicle destroyed: {}", chronicle_name);

    // Assertion for successful chronicle destruction or expected errors
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED ||
           ret == chronolog::CL_ERR_NO_CONNECTION);


    ret = client->Disconnect();
    delete client;

    return 0;
}
