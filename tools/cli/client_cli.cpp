#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <pwd.h>
#include <getopt.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <margo.h>

#include <chronolog_client.h>
#include <ClientConfiguration.h>
#include <chrono_monitor.h>
#include <cmd_arg_parse.h>
#include <common.h>

// Interactive ChronoLog client shell.
// Extracted from client_admin.cpp; the scripted/perf-test workload lives in
// test/performance/performance_test.cpp.

static void usage(char** argv)
{
    std::cerr << "\nUsage: " << argv[0]
              << " [options]\n"
                 "-c|--config <config_file>\n"
                 "-u|--usage\t\tPrint this page\n"
              << std::endl;
}

static std::string& trim_string(std::string& str)
{
    // Trim leading space
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    // Trim trailing space
    str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
              str.end());
    return str;
}

static uint64_t get_uint64_t_from_string(const std::string& str)
{
    char* endptr;
    errno = 0; // Reset errno before calling strtoull
    uint64_t value = strtoull(str.c_str(), &endptr, 10);
    if(*endptr != '\0')
    {
        std::cerr << "Invalid number: " << str << std::endl;
        exit(EXIT_FAILURE);
    }
    if(value == 0 || errno == ERANGE)
    {
        std::cerr << "Only positive number is allowed" << std::endl;
        exit(EXIT_FAILURE);
    }
    return value;
}

static std::string parse_config_arg(int argc, char** argv)
{
    int opt;
    char* config_file = nullptr;

    struct option long_options[] = {{"config", required_argument, nullptr, 'c'},
                                    {"usage", no_argument, nullptr, 'u'},
                                    {nullptr, 0, nullptr, 0}};

    while((opt = getopt_long(argc, argv, "c:u", long_options, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                config_file = optarg;
                break;
            case 'u':
                usage(argv);
                exit(EXIT_SUCCESS);
            case '?':
            default:
                usage(argv);
                exit(EXIT_FAILURE);
        }
    }

    return config_file ? std::string(config_file) : std::string();
}

static int test_create_chronicle(chronolog::Client& client, const std::string& chronicle_name)
{
    int ret, flags = 0;
    std::map<std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    chronicle_attrs.emplace("IndexGranularity", "Millisecond");
    chronicle_attrs.emplace("TieringPolicy", "Hot");
    ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_CHRONICLE_EXISTS);
    return ret;
}

static std::pair<int, chronolog::StoryHandle*>
test_acquire_story(chronolog::Client& client, const std::string& chronicle_name, const std::string& story_name)
{
    int flags = 0;
    std::map<std::string, std::string> story_acquisition_attrs;
    story_acquisition_attrs.emplace("Priority", "High");
    story_acquisition_attrs.emplace("IndexGranularity", "Millisecond");
    story_acquisition_attrs.emplace("TieringPolicy", "Hot");
    return client.AcquireStory(chronicle_name, story_name, story_acquisition_attrs, flags);
}

static uint64_t test_write_event(chronolog::StoryHandle* story_handle, const std::string& event_payload)
{
    uint64_t ret = story_handle->log_event(event_payload);
    assert(ret > 0);
    return ret;
}

static int test_replay_story(chronolog::Client& client,
                             const std::string& chronicle_name,
                             const std::string& story_name,
                             uint64_t start_time,
                             uint64_t end_time,
                             std::vector<chronolog::Event>& replay_events)
{
    int ret = client.ReplayStory(chronicle_name, story_name, start_time, end_time, replay_events);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST);
    return ret;
}

static int
test_release_story(chronolog::Client& client, const std::string& chronicle_name, const std::string& story_name)
{
    int ret = client.ReleaseStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST);
    return ret;
}

static int test_destroy_chronicle(chronolog::Client& client, const std::string& chronicle_name)
{
    int ret = client.DestroyChronicle(chronicle_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
    return ret;
}

static int
test_destroy_story(chronolog::Client& client, const std::string& chronicle_name, const std::string& story_name)
{
    int ret = client.DestroyStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
    return ret;
}

static std::vector<std::string> parse_command_line(const std::string& line)
{
    std::vector<std::string> tokens;
    std::string current_token;
    bool in_quotes = false;

    for(char ch: line)
    {
        if(ch == '\"')
        {
            if(in_quotes)
            {
                tokens.push_back(current_token);
                current_token.clear();
                in_quotes = false;
            }
            else
            {
                if(!current_token.empty())
                {
                    tokens.push_back(current_token);
                    current_token.clear();
                }
                in_quotes = true;
            }
        }
        else if(std::isspace(static_cast<unsigned char>(ch)) && !in_quotes)
        {
            if(!current_token.empty())
            {
                tokens.push_back(current_token);
                current_token.clear();
            }
        }
        else
        {
            current_token += ch;
        }
    }

    if(!current_token.empty())
    {
        tokens.push_back(current_token);
    }

    if(in_quotes)
    {
        std::cerr << "Error: Unterminated quote in command line: " << line << std::endl;
        tokens.clear();
        return tokens;
    }

    return tokens;
}

static void
command_dispatcher(const std::string& command_line,
                   std::unordered_map<std::string, std::function<void(std::vector<std::string>&)>> command_map)
{
    if(command_line.empty())
    {
        return;
    }

    std::vector<std::string> parsed_tokens = parse_command_line(command_line);

    if(parsed_tokens.empty())
    {
        return;
    }

    std::string command_name = trim_string(parsed_tokens[0]);
    auto it = command_map.find(command_name);
    if(it != command_map.end())
    {
        it->second(parsed_tokens);
    }
    else
    {
        std::cerr << "Error: Command not found: '" << parsed_tokens[0] << "'" << std::endl;
    }
}

static void interactive_create_chronicle(std::vector<std::string>& tokens, chronolog::Client& client)
{
    if(tokens.size() != 2)
    {
        std::cerr << "Usage: -c <chronicle_name>" << std::endl;
        return;
    }
    int ret_i;
    const std::string& chronicle_name = tokens[1];
    ret_i = test_create_chronicle(client, chronicle_name);
    if(ret_i == chronolog::CL_SUCCESS)
    {
        std::cout << "Chronicle created successfully: " << chronicle_name << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        std::cout << "Chronicle already exists: " << chronicle_name << std::endl;
    }
    else
    {
        std::cout << "Failed to create Chronicle: " << chronicle_name
                  << ", return code: " << chronolog::to_string_client(ret_i) << std::endl;
    }
}

static chronolog::StoryHandle* interactive_acquire_story(std::vector<std::string>& tokens, chronolog::Client& client)
{
    if(tokens.size() != 4)
    {
        std::cerr << "Usage: -a -s <chronicle_name> <story_name>" << std::endl;
        return nullptr;
    }
    int ret_i;
    chronolog::StoryHandle* story_handle = nullptr;
    const std::string& chronicle_name = tokens[2];
    const std::string& story_name = tokens[3];
    auto ret = test_acquire_story(client, chronicle_name, story_name);
    ret_i = ret.first;
    story_handle = ret.second;
    if(ret_i == chronolog::CL_SUCCESS)
    {
        std::cout << "Story acquired successfully: " << story_name << " in Chronicle " << chronicle_name << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_ACQUIRED)
    {
        std::cout << "Story already acquired: " << story_name << " in Chronicle " << chronicle_name << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_NOT_EXIST)
    {
        std::cout << "Chronicle does not exist: " << chronicle_name << std::endl;
    }
    else
    {
        std::cout << "Failed to acquire story: " << story_name << " in Chronicle " << chronicle_name
                  << ", return code: " << chronolog::to_string_client(ret_i) << std::endl;
    }
    return story_handle;
}

static void interactive_release_story(std::vector<std::string>& tokens, chronolog::Client& client)
{
    if(tokens.size() != 4)
    {
        std::cerr << "Usage: -q -s <chronicle_name> <story_name>" << std::endl;
        return;
    }
    int ret_i;
    const std::string& chronicle_name = tokens[2];
    const std::string& story_name = tokens[3];
    ret_i = test_release_story(client, chronicle_name, story_name);
    if(ret_i == chronolog::CL_SUCCESS)
    {
        std::cout << "Story released successfully: " << story_name << " in Chronicle " << chronicle_name << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_NOT_EXIST)
    {
        std::cout << "Story does not exist: " << story_name << " in Chronicle " << chronicle_name << std::endl;
    }
    else
    {
        std::cout << "Failed to release story: " << story_name << " in Chronicle " << chronicle_name
                  << ", return code: " << chronolog::to_string_client(ret_i) << std::endl;
    }
}

static void interactive_write_event(std::vector<std::string>& tokens, chronolog::StoryHandle* story_handle)
{
    if(tokens.size() < 2)
    {
        std::cerr << "Usage: -w <event_payload>" << std::endl;
        return;
    }
    uint64_t ret_u;
    std::string event_payload;
    for(size_t i = 1; i < tokens.size(); ++i)
    {
        if(i > 1)
        {
            event_payload += " ";
        }
        event_payload += tokens[i];
    }
    ret_u = test_write_event(story_handle, event_payload);
    if(ret_u > 0)
    {
        std::cout << "Event written successfully, payload length: " << event_payload.length() << std::endl;
    }
    else
    {
        std::cout << "Failed to write event, return code: " << ret_u << std::endl;
    }
}

static void interactive_replay_story(std::vector<std::string>& tokens, chronolog::Client& client)
{
    if(tokens.size() != 5)
    {
        std::cerr << "Usage: -r <chronicle_name> <story_name> <start_time> <end_time>" << std::endl;
        return;
    }
    int ret_i;
    const std::string& chronicle_name = tokens[1];
    const std::string& story_name = tokens[2];
    uint64_t start_time = get_uint64_t_from_string(tokens[3]);
    uint64_t end_time = get_uint64_t_from_string(tokens[4]);
    std::vector<chronolog::Event> replay_events;
    ret_i = test_replay_story(client, chronicle_name, story_name, start_time, end_time, replay_events);
    if(ret_i == chronolog::CL_SUCCESS)
    {
        std::cout << "Replay successful, retrieved " << replay_events.size() << " events from " << chronicle_name
                  << " story " << story_name << " between " << start_time << " and " << end_time << std::endl;
        for(const auto& event: replay_events) { std::cout << event.to_string() << std::endl; }
    }
    else if(ret_i == chronolog::CL_ERR_NOT_EXIST)
    {
        std::cout << "Story does not exist: " << story_name << " in Chronicle " << chronicle_name << std::endl;
    }
    else
    {
        std::cout << "Failed to replay story: " << story_name << " in Chronicle " << chronicle_name
                  << ", return code: " << chronolog::to_string_client(ret_i) << std::endl;
    }
}

static void interactive_destroy_story(std::vector<std::string>& tokens, chronolog::Client& client)
{
    if(tokens.size() != 4)
    {
        std::cerr << "Usage: -d -s <chronicle_name> <story_name>" << std::endl;
        return;
    }
    int ret_i;
    const std::string& chronicle_name = tokens[2];
    const std::string& story_name = tokens[3];
    ret_i = test_destroy_story(client, chronicle_name, story_name);
    if(ret_i == chronolog::CL_SUCCESS)
    {
        std::cout << "Story destroyed successfully: " << story_name << " in Chronicle " << chronicle_name << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_ACQUIRED)
    {
        std::cout << "Story is still acquired, cannot destroy: " << story_name << " in Chronicle " << chronicle_name
                  << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_NOT_EXIST)
    {
        std::cout << "Story does not exist: " << story_name << " in Chronicle " << chronicle_name << std::endl;
    }
    else
    {
        std::cout << "Failed to destroy Story: " << story_name << " in Chronicle " << chronicle_name
                  << ", return code: " << chronolog::to_string_client(ret_i) << std::endl;
    }
}

static void interactive_destroy_chronicle(std::vector<std::string>& tokens, chronolog::Client& client)
{
    if(tokens.size() != 3)
    {
        std::cerr << "Usage: -d -c <chronicle_name>" << std::endl;
        return;
    }
    int ret_i;
    const std::string& chronicle_name = tokens[2];
    ret_i = test_destroy_chronicle(client, chronicle_name);
    if(ret_i == chronolog::CL_SUCCESS)
    {
        std::cout << "Chronicle destroyed successfully: " << chronicle_name << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_ACQUIRED)
    {
        std::cout << "Chronicle is still acquired, cannot destroy: " << chronicle_name << std::endl;
    }
    else if(ret_i == chronolog::CL_ERR_NOT_EXIST)
    {
        std::cout << "Chronicle does not exist: " << chronicle_name << std::endl;
    }
    else
    {
        std::cout << "Failed to destroy Chronicle: " << chronicle_name
                  << ", return code: " << chronolog::to_string_client(ret_i) << std::endl;
    }
}

int main(int argc, char** argv)
{
    // To suppress argobots warning
    std::string argobots_conf_str = R"({"argobots" : {"abt_mem_max_num_stacks" : 8
                                                    , "abt_thread_stacksize" : 2097152}})";
    margo_set_environment(argobots_conf_str.c_str());

    std::string conf_file_path = parse_config_arg(argc, argv);

    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
    {
        if(!confManager.load_from_file(conf_file_path))
        {
            std::cerr << "[ClientCLI] Failed to load configuration file '" << conf_file_path
                      << "'. Using default values instead." << std::endl;
        }
        else
        {
            std::cout << "[ClientCLI] Configuration file loaded successfully from '" << conf_file_path << "'."
                      << std::endl;
        }
    }
    else
    {
        std::cout << "[ClientCLI] No configuration file provided. Using default values." << std::endl;
    }

    confManager.log_configuration(std::cout);

    // Initialize logging
    int result = chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                                       confManager.LOG_CONF.LOGFILE,
                                                       confManager.LOG_CONF.LOGLEVEL,
                                                       confManager.LOG_CONF.LOGNAME,
                                                       confManager.LOG_CONF.LOGFILESIZE,
                                                       confManager.LOG_CONF.LOGFILENUM,
                                                       confManager.LOG_CONF.FLUSHLEVEL);

    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    chronolog::Client client(confManager.PORTAL_CONF, confManager.QUERY_CONF);
    chronolog::StoryHandle* story_handle = nullptr;

    int ret_i;

    std::string client_id = gen_random(8);

    const std::string& server_protoc = confManager.PORTAL_CONF.PROTO_CONF;
    const std::string& server_ip = confManager.PORTAL_CONF.IP;
    std::string server_port = std::to_string(confManager.PORTAL_CONF.PORT);
    std::string server_provider_id = std::to_string(confManager.PORTAL_CONF.PROVIDER_ID);
    std::string server_address = server_protoc + "://" + server_ip + ":" + server_port + "@" + server_provider_id;

    std::string username = getpwuid(getuid())->pw_name;
    ret_i = client.Connect();
    assert(ret_i == chronolog::CL_SUCCESS);

    std::cout << "Connected to server address: " << server_address << std::endl;

    std::cout << "Metadata operations: \n"
              << "\t-c <chronicle_name> , create a Chronicle <chronicle_name>\n"
              << "\t-a -s <chronicle_name> <story_name>, acquire Story <story_name> in Chronicle <chronicle_name>\n"
              << "\t-w <event_string>, write Event with <event_string> as payload\n"
              << "\t-r <chronicle_name> <story_name> <start_time> <end_time>, read Events in Story <story_name> of "
                 "Chronicle <chronicle_name> from <start_time> to <end_time>\n"
              << "\t-q -s <chronicle_name> <story_name>, release Story <story_name> in Chronicle <chronicle_name>\n"
              << "\t-d -s <chronicle_name> <story_name>, destroy Story <story_name> in Chronicle <chronicle_name>\n"
              << "\t-d -c <chronicle_name>, destroy Chronicle <chronicle_name>\n"
              << "\t-disconnect\n"
              << std::endl;

    std::unordered_map<std::string, std::function<void(std::vector<std::string>&)>> command_map = {
            {"-c", [&](std::vector<std::string>& command_subs) { interactive_create_chronicle(command_subs, client); }},
            {"-a",
             [&](std::vector<std::string>& command_subs)
             {
                 if(command_subs[1] == "-s")
                 {
                     story_handle = interactive_acquire_story(command_subs, client);
                 }
             }},
            {"-q",
             [&](std::vector<std::string>& command_subs)
             {
                 if(command_subs[1] == "-s")
                 {
                     interactive_release_story(command_subs, client);
                 }
             }},
            {"-w",
             [&](std::vector<std::string>& command_subs) { interactive_write_event(command_subs, story_handle); }},
            {"-r", [&](std::vector<std::string>& command_subs) { interactive_replay_story(command_subs, client); }},
            {"-d",
             [&](std::vector<std::string>& command_subs)
             {
                 if(command_subs[1] == "-c")
                 {
                     interactive_destroy_chronicle(command_subs, client);
                 }
                 else if(command_subs[1] == "-s")
                 {
                     interactive_destroy_story(command_subs, client);
                 }
             }}};

    std::string command_line;
    while(true)
    {
        command_line.clear();
        std::getline(std::cin, command_line);
        command_line = trim_string(command_line);
        if(command_line == "-disconnect")
            break;
        command_dispatcher(command_line, command_map);
    }

    ret_i = client.Disconnect();
    assert(ret_i == chronolog::CL_SUCCESS);

    return 0;
}
