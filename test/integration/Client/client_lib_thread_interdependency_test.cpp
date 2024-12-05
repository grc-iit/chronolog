#include <chronolog_client.h>
#include <common.h>
#include <thread>
#include <chrono>
#include <cmd_arg_parse.h>
#include "chrono_monitor.h"
#include <vector>
#include <bitset>

#define STORY_NAME_LEN 5

struct thread_arg
{
    int tid;
    std::string chronicle_name;
    std::string story_name;
};

chronolog::Client*client;
std::vector<struct thread_arg> t_args;
std::vector <int> shared_state;
std::mutex shared_state_mutex;

enum class ThreadState
{
    UNKNOWN = 0,
    THREAD_INITIALIZED = 1,
    CHRONICLE_CREATED = 2,
    STORY_ACQUIRED = 3,
    STORY_RELEASED = 4,
    STORY_DESTROYED = 5,
    CHRONICLE_DESTROYED = 6,
    THREAD_FINALIZED = 7
};

std::string get_state_name(ThreadState state)
{
    switch(state)
    {
        case ThreadState::THREAD_FINALIZED:
            return "THREAD_FINALIZED";
        case ThreadState::CHRONICLE_DESTROYED:
            return "CHRONICLE_DESTROYED";
        case ThreadState::STORY_DESTROYED:
            return "STORY_DESTROYED";
        case ThreadState::STORY_RELEASED:
            return "STORY_RELEASED";
        case ThreadState::STORY_ACQUIRED:
            return "STORY_ACQUIRED";
        case ThreadState::CHRONICLE_CREATED:
            return "CHRONICLE_CREATED";
        case ThreadState::THREAD_INITIALIZED:
            return "THREAD_INITIALIZED";
        case ThreadState::UNKNOWN:
            return "UNKNOWN";
        default:
            return "UNKNOWN_STATE";
    }
}

void update_chronicle_threads_state(int tid, ThreadState new_state)
{
    for(size_t i = 0; i < shared_state.size(); ++i)
    {
        if(t_args[i].tid == tid) continue;
        if((t_args[i].chronicle_name == t_args[tid].chronicle_name))
        {
            shared_state[i] = static_cast<int>(new_state);
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- Thread {} state updated to: {}", tid, i
                     , get_state_name(new_state));
        }
    }
}

void update_story_threads_state(int tid, ThreadState new_state)
{
    for(size_t i = 0; i < shared_state.size(); ++i)
    {
        if(t_args[i].tid == tid) continue;
        if((t_args[i].chronicle_name == t_args[tid].chronicle_name) && (t_args[i].story_name == t_args[tid].story_name))
        {
            shared_state[i] = static_cast<int>(new_state);  // Update state
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- Thread {} state updated to: {}", tid, i
                     , get_state_name(new_state));
        }
    }
}

void check_thread_initialization(int tid, int ret)
{
    std::lock_guard <std::mutex> lock(shared_state_mutex);
    if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::UNKNOWN)
    {
        shared_state[tid] = static_cast<int>(ThreadState::THREAD_INITIALIZED);
        LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- State updated to: {}", tid, get_state_name(
                ThreadState::THREAD_INITIALIZED));
    }
}

void check_chronicle_created(int tid, int ret)
{
    std::lock_guard <std::mutex> lock(shared_state_mutex);
    if(ret == chronolog::CL_SUCCESS)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::THREAD_INITIALIZED)
        {
            shared_state[tid] = static_cast<int>(ThreadState::CHRONICLE_CREATED);
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- State updated to: {}", tid, get_state_name(
                    ThreadState::CHRONICLE_CREATED));
            update_chronicle_threads_state(tid, ThreadState::CHRONICLE_CREATED);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_SUCCESS when transitioning to "
                      "CHRONICLE_CREATED state from a state different than THREAD_INITIALIZED", tid);
        }
    }
    else if(ret == chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        if(static_cast<ThreadState>(shared_state[tid]) != ThreadState::UNKNOWN &&
           static_cast<ThreadState>(shared_state[tid]) != ThreadState::THREAD_INITIALIZED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_CHRONICLE_EXISTS return "
                     "value when trying to create a chronicle on a state different from UNKNOWN or THREAD_INITIALIZED"
                     , tid);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_CHRONICLE_EXISTS return value"
                      " when trying to create a chronicle on the UNKNOWN or THREAD_INITIALIZED state", tid);
        }
    }
    else
    {
        LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- the return value is an unexpected one", tid);
    }
}

void check_story_acquired(int tid, int ret)
{
    std::lock_guard <std::mutex> lock(shared_state_mutex);
    if(ret == chronolog::CL_SUCCESS)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::CHRONICLE_CREATED ||
           static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_RELEASED ||
           static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_DESTROYED)
        {
            shared_state[tid] = static_cast<int>(ThreadState::STORY_ACQUIRED);
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- State updated to: {}", tid, get_state_name(
                    ThreadState::STORY_ACQUIRED));
            update_story_threads_state(tid, ThreadState::STORY_ACQUIRED);
        }
        else if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_ACQUIRED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_SUCCESS return value when trying"
                     " to acquire a Story on a STORY_ACQUIRED", tid);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_SUCCESS return value when trying "
                      "to acquire a story on a state different than CHRONICLE_CREATED, STORY_RELEASED, STORY_DESTROYED "
                      "or STORY_ACQUIRED", tid);
        }
    }
    else if(ret == chronolog::CL_ERR_NOT_EXIST)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::THREAD_INITIALIZED ||
           static_cast<ThreadState>(shared_state[tid]) == ThreadState::CHRONICLE_DESTROYED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_EXIST return value when "
                     "trying to acquire a Story on a THREAD_INITIALIZED or CHRONICLE_DESTROYED", tid);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_EXIST return value when "
                      "trying to acquire a story on the THREAD_INITIALIZED or CHRONICLE_DESTROYED state", tid);
        }
    }
    else
    {
        LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- the return value is an unexpected one", tid);
    }
}

void check_story_released(int tid, int ret)
{
    std::lock_guard <std::mutex> lock(shared_state_mutex);
    if(ret == chronolog::CL_SUCCESS)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_ACQUIRED)
        {
            shared_state[tid] = static_cast<int>(ThreadState::STORY_RELEASED);
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- State updated to: {}", tid, get_state_name(
                    ThreadState::STORY_RELEASED));
            update_story_threads_state(tid, ThreadState::STORY_RELEASED);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_SUCCESS return value when "
                      "trying to release a story on the a STORY_RELEASED state", tid);
        }
    }
    else if(ret == chronolog::CL_ERR_NOT_ACQUIRED)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_RELEASED ||
           static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_DESTROYED ||
           static_cast<ThreadState>(shared_state[tid]) == ThreadState::CHRONICLE_DESTROYED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_ACQUIRED return value "
                     "when trying to release a Story on a STORY_RELEASED,STORY_DESTROYED or CHRONICLE_DESTROYED state", tid);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_ACQUIRED return value "
                      "when trying to release a story on the a state different from STORY_RELEASED, STORY_DESTROYED or"
                      " CHRONICLE_DESTROYED", tid);
        }
    }
    else
    {
        LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- the return value is an unexpected one", tid);
    }
}

void check_story_destroyed(int tid, int ret)
{
    std::lock_guard <std::mutex> lock(shared_state_mutex);

    if(ret == chronolog::CL_SUCCESS)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_RELEASED)
        {
            shared_state[tid] = static_cast<int>(ThreadState::STORY_DESTROYED);
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- State updated to: {}", tid, get_state_name(
                    ThreadState::STORY_DESTROYED));
            update_story_threads_state(tid, ThreadState::STORY_DESTROYED);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_SUCCESS return value when"
                      " trying to destroy a story on a state different from STORY_RELEASED", tid);
        }
    }
    else if(ret == chronolog::CL_ERR_ACQUIRED)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_ACQUIRED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_ACQUIRED return value when "
                     "trying to destroy a Story on the STORY_ACQUIRED state.", tid);
        }
        else
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_ACQUIRED return value when "
                     "trying to destroy a story on a state different from STORY_ACQUIRED state.", tid);
        }
    }
    else if(ret == chronolog::CL_ERR_NOT_EXIST)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_DESTROYED ||
           static_cast<ThreadState>(shared_state[tid]) == ThreadState::CHRONICLE_DESTROYED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_EXIST return value when "
                     " trying to destroy a Story on the STORY_DESTROYED or CHRONICLE_DESTROYED state.", tid);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_EXIST return value when "
                      "trying to destroy a story on a state different than STORY_DESTROYED or CHRONICLE_DESTROYED "
                      "state.", tid);
        }
    }
    else
    {
        LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- the return value is an unexpected one", tid);
    }
}

void check_chronicle_destroyed(int tid, int ret)
{
    std::lock_guard <std::mutex> lock(shared_state_mutex);
    if(ret == chronolog::CL_SUCCESS)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_RELEASED ||
           static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_DESTROYED)
        {
            shared_state[tid] = static_cast<int>(ThreadState::CHRONICLE_DESTROYED);
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- State updated to: {}", tid, get_state_name(
                    ThreadState::CHRONICLE_DESTROYED));
            update_chronicle_threads_state(tid, ThreadState::CHRONICLE_DESTROYED);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_SUCCESS return value when trying "
                      "to destroy chronicle on a state different than STORY_RELEASED", tid);
        }
    }
    else if(ret == chronolog::CL_ERR_ACQUIRED)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::STORY_ACQUIRED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_ACQUIRED return value when "
                     "trying to destroy chronicle on STORY_ACQUIRED state", tid);
        }
        else
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_ACQUIRED return value when "
                     "trying to destroy chronicle on a state different than STORY_ACQUIRED", tid);
        }
    }
    else if(ret == chronolog::CL_ERR_NOT_EXIST)
    {
        if(static_cast<ThreadState>(shared_state[tid]) == ThreadState::CHRONICLE_DESTROYED)
        {
            LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_EXIST return value when "
                     "trying to destroy chronicle on a state STORY_DESTROYED or CHRONICLE_DESTROYED", tid);
        }
        else
        {
            LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- received a CL_ERR_NOT_EXIST return value when "
                      "trying to destroy chronicle on a state different than STORY_DESTROYED or CHRONICLE_DESTROYED"
                      , tid);
        }
    }
    else
    {
        LOG_ERROR("[ClientLibThreadInterdependencyTest] -Thread {}- the return value is an unexpected one", tid);
    }
}

void check_thread_finalized(int tid, int ret)
{
    std::lock_guard <std::mutex> lock(shared_state_mutex);
    shared_state[tid] = static_cast<int>(ThreadState::THREAD_FINALIZED);
    LOG_INFO("[ClientLibThreadInterdependencyTest] -Thread {}- State updated to: {}", tid, get_state_name(
            ThreadState::THREAD_FINALIZED));
}

void thread_body(struct thread_arg*t)
{
    // Thread Initialized
    check_thread_initialization(t->tid, 0);


    // Chronicle Variables
    std::map <std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    int flags = 1;
    // Chronicle creation
    int ret = client->CreateChronicle(t->chronicle_name, chronicle_attrs, flags);
    LOG_INFO("[ClientLibThreadInterdependencyTest] Chronicle created: tid={}, Ret: {}", t->tid, ret);
    check_chronicle_created(t->tid, ret);


    // Story Variables
    std::map <std::string, std::string> story_attrs;
    flags = 2;
    // Acquire story
    auto acquire_ret = client->AcquireStory(t->chronicle_name, t->story_name, story_attrs, flags);
    LOG_INFO("[ClientLibThreadInterdependencyTest] Story acquired: tid={}, Ret: {}", t->tid, acquire_ret.first);
    check_story_acquired(t->tid, acquire_ret.first);


    // Release the story
    ret = client->ReleaseStory(t->chronicle_name, t->story_name);
    LOG_INFO("[ClientLibThreadInterdependencyTest] Story released: tid={}, Ret: {}", t->tid, ret);
    check_story_released(t->tid, ret);


    // Destroy the story
    ret = client->DestroyStory(t->chronicle_name, t->story_name);
    LOG_INFO("[ClientLibThreadInterdependencyTest] Story destroyed: tid={}, Ret: {}", t->tid, ret);
    check_story_destroyed(t->tid, ret);


    // Destroy the chronicle
    ret = client->DestroyChronicle(t->chronicle_name);
    LOG_INFO("[ClientLibThreadInterdependencyTest] Chronicle destroyed: tid={}, Ret: {}", t->tid, ret);
    check_chronicle_destroyed(t->tid, ret);


    // Thread Finalized
    check_thread_finalized(t->tid, 0);
}


int main(int argc, char**argv)
{
    // Configuration
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
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
        std::exit(EXIT_FAILURE);
    }

    // Set up client & Connect
    client = new chronolog::Client(confManager);
    int ret = client->Connect();
    if(chronolog::CL_SUCCESS != ret)
    {
        LOG_ERROR("[ClientLibThreadInterdependencyTest] Failed to connect to ChronoVisor");
        delete client;
        return -1;
    }

    // Initiate test
    LOG_INFO("[ClientLibThreadInterdependencyTest] Running test.");
    int num_threads = 8;
    t_args.resize(num_threads);
    std::vector <std::thread> workers(num_threads);
    shared_state.resize(num_threads, 0);

    // Create and start the worker threads
    for(int i = 0; i < num_threads; i++)
    {
        t_args[i].tid = i;  // Assign thread ID
        t_args[i].chronicle_name = (i % 2 == 0) ? "CHRONICLE_2" : "CHRONICLE_1";  // Assign Chronicle Name
        t_args[i].story_name = (i % 4 < 2) ? "STORY_1" : "STORY_2";  // Assign Story Name
        std::thread t{thread_body, &t_args[i]};  // Start the thread
        workers[i] = std::move(t);  // Move thread to workers vector
    }

    // Join all worker threads to wait for their completion
    for(int i = 0; i < num_threads; i++)
    {
        workers[i].join();
    }

    LOG_INFO("[ClientLibThreadInterdependencyTest] End of the test.");

    // Disconnect the client and clean up
    ret = client->Disconnect();
    delete client;

    // Return success
    return 0;
}