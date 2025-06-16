#include <chronolog_client.h>
#include <cmd_arg_parse.h>
#include <common.h>
#include <cassert>
#include <unistd.h>
#include <pwd.h>
#include <getopt.h>
#include <functional>
#include <chrono>
#include <cctype>
#include <mpi.h>
#include <chrono_monitor.h>
#include <TimerWrapper.h>
#include <cstring>
#include <margo.h>

#define MAX_EVENT_SIZE (32 * 1024 * 1024)

typedef struct workload_conf_args_
{
    uint64_t chronicle_count = 1;
    uint64_t story_count = 1;
    uint64_t min_event_size = 0;
    uint64_t ave_event_size = 256;
    uint64_t max_event_size = 512;
    uint64_t event_count = 1;
    uint64_t event_interval = 0;
    bool barrier = false;
    std::string event_payload_file;
    bool interactive = false;
    bool shared_story = false;
    bool perf_test = false;
} workload_conf_args;

void usage(char**argv)
{
    std::cerr << "\nUsage: " << argv[0] << " [options]\n"
                 "-c|--config <config_file>\n"
                 "-i|--interactive\n"
                 "-h|--chronicle_count <chronicle_count>\n"
                 "-t|--story_count <story_count>\n"
                 "-a|--min_event_size <min_event_size>\n"
                 "-s|--ave_event_size <ave_event_size>\n"
                 "-b|--max_event_size <max_event_size>\n"
                 "-n|--event_count <event_count>\n"
                 "-g|--event_interval <event_interval>\n"
                 "-r|--barrier\n"
                 "-f|--event_payload_file <event_payload_file>\n"
                 "-o|--shared_story\n"
                 "-p|--perf\n" << argv[0] << std::endl;
}

void random_sleep()
{
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    srand(getpid());
    long usec = random() % 100000;
    LOG_DEBUG("Sleeping for {} us ...", usec);
    usleep(usec * rank);
}

std::string &trim_string(std::string &str)
{
    // Trim leading space
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch)
    {
        return !std::isspace(ch);
    }));
    // Trim trailing space
    str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch)
    {
        return !std::isspace(ch);
    }).base(), str.end());
    return str;
}

uint64_t get_uint64_t_from_string(const std::string &str)
{
    char *endptr;
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

std::pair <std::string, workload_conf_args> cmd_arg_parse(int argc, char**argv)
{
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    int opt;
    char*config_file = nullptr;
    workload_conf_args workload_args;

    // Define the long options and their corresponding short options
    struct option long_options[] = {{  "config"              , required_argument, nullptr, 'c'}
                                    , {"interactive"         , optional_argument, nullptr, 'i'}
                                    , {"chronicle_count"     , required_argument, nullptr, 'h'}
                                    , {"story_count"         , required_argument, nullptr, 't'}
                                    , {"min_event_size"      , required_argument, nullptr, 'a'}
                                    , {"ave_event_size"      , required_argument, nullptr, 's'}
                                    , {"max_event_size"      , required_argument, nullptr, 'b'}
                                    , {"event_count"         , required_argument, nullptr, 'n'}
                                    , {"event_interval"      , required_argument, nullptr, 'g'}
                                    , {"barrier"             , optional_argument, nullptr, 'r'}
                                    , {"event_payload_file"  , optional_argument, nullptr, 'f'}
                                    , {"shared_story"        , optional_argument, nullptr, 'o'}
                                    , {"perf"                , optional_argument, nullptr, 'p'}
                                    , {nullptr               , 0                , nullptr, 0}
    };

    // Parse the command-line options
    while((opt = getopt_long(argc, argv, "c:ih:t:a:s:b:n:g:rf:op", long_options, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                config_file = optarg;
                break;
            case 'i':
                workload_args.interactive = true;
                break;
            case 'h':
                workload_args.chronicle_count = get_uint64_t_from_string(optarg);
                break;
            case 't':
                workload_args.story_count = get_uint64_t_from_string(optarg);
                break;
            case 'a':
                workload_args.min_event_size = get_uint64_t_from_string(optarg);
                break;
            case 's':
                workload_args.ave_event_size = get_uint64_t_from_string(optarg);
                break;
            case 'b':
                workload_args.max_event_size = get_uint64_t_from_string(optarg);
                break;
            case 'n':
                workload_args.event_count = get_uint64_t_from_string(optarg);
                break;
            case 'g':
                workload_args.event_interval = get_uint64_t_from_string(optarg);
                break;
            case 'r':
                workload_args.barrier = true;
                break;
            case 'f':
                workload_args.event_payload_file = optarg;
                break;
            case 'o':
                workload_args.shared_story = true;
                break;
            case 'p':
                workload_args.perf_test = true;
                break;
            case '?':
                // Invalid option or missing argument
                usage(argv);
                exit(EXIT_FAILURE);
            default:
                // Unknown option
                std::cerr << "Unknown option: " << opt << std::endl;
                exit(EXIT_FAILURE);
        }
    }

    // Check if the config file option is provided
    if(config_file)
    {
        if(rank == 0)
        {
            std::cout << "Config file specified: " << config_file << std::endl;
            if(workload_args.interactive)
            {
                std::cout << "Interactive mode: on" << std::endl;
            }
            else
            {
                std::cout << "Interactive mode: off" << std::endl;
                std::cout << "Chronicle count: " << workload_args.chronicle_count << std::endl;
                std::cout << "Story count: " << workload_args.story_count << std::endl;
                if(!workload_args.event_payload_file.empty())
                {
                    std::cout << "Event payload file specified: " << workload_args.event_payload_file.c_str()
                              << std::endl;
                    std::cout << "Barrier: " << (workload_args.barrier ? "true" : "false") << std::endl;
                    std::cout << "Shared story: " << (workload_args.shared_story ? "true" : "false") << std::endl;
                }
                else
                {
                    std::cout << "No event input file specified, use default/specified separate conf args ..."
                              << std::endl;
                    std::cout << "Min event size: " << workload_args.min_event_size << std::endl;
                    std::cout << "Ave event size: " << workload_args.ave_event_size << std::endl;
                    std::cout << "Max event size: " << workload_args.max_event_size << std::endl;
                    std::cout << "Event count: " << workload_args.event_count << std::endl;
                    std::cout << "Event interval: " << workload_args.event_interval << std::endl;
                    std::cout << "Barrier: " << (workload_args.barrier ? "true" : "false") << std::endl;
                    std::cout << "Shared story: " << (workload_args.shared_story ? "true" : "false") << std::endl;
                }
            }
        }
        return {std::pair <std::string, workload_conf_args>((config_file), workload_args)};
    }
    else
    {
        if(rank == 0)
        {
            std::cout << "No config file specified, using default settings instead:" << std::endl;
            std::cout << "Interactive mode: " << (workload_args.interactive ? "on" : "off") << std::endl;
            std::cout << "Chronicle count: " << workload_args.chronicle_count << std::endl;
            std::cout << "Story count: " << workload_args.story_count << std::endl;
            std::cout << "Min event size: " << workload_args.min_event_size << std::endl;
            std::cout << "Ave event size: " << workload_args.ave_event_size << std::endl;
            std::cout << "Max event size: " << workload_args.max_event_size << std::endl;
            std::cout << "Event count: " << workload_args.event_count << std::endl;
            std::cout << "Event interval: " << workload_args.event_interval << std::endl;
            std::cout << "Barrier: " << (workload_args.barrier ? "true" : "false") << std::endl;
            std::cout << "Shared story: " << (workload_args.shared_story ? "true" : "false") << std::endl;
        }
        return {};
    }
}

int test_create_chronicle(chronolog::Client &client, const std::string &chronicle_name)
{
    int ret, flags = 0;
    std::map <std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    chronicle_attrs.emplace("IndexGranularity", "Millisecond");
    chronicle_attrs.emplace("TieringPolicy", "Hot");
    ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_CHRONICLE_EXISTS);
    return ret;
}

chronolog::StoryHandle*
test_acquire_story(chronolog::Client &client, const std::string &chronicle_name, const std::string &story_name)
{
//    random_sleep();
    int flags = 0;
    std::map <std::string, std::string> story_acquisition_attrs;
    story_acquisition_attrs.emplace("Priority", "High");
    story_acquisition_attrs.emplace("IndexGranularity", "Millisecond");
    story_acquisition_attrs.emplace("TieringPolicy", "Hot");
    std::pair <int, chronolog::StoryHandle*> acq_ret = client.AcquireStory(chronicle_name, story_name
                                                                           , story_acquisition_attrs, flags);
//    LOG_DEBUG("acq_ret: {}", to_string_client(acq_ret.first));
    assert(acq_ret.first == chronolog::CL_SUCCESS || acq_ret.first == chronolog::CL_ERR_ACQUIRED);
    return acq_ret.second;
}

uint64_t test_write_event(chronolog::StoryHandle*story_handle, const std::string &event_payload)
{
    uint64_t ret = story_handle->log_event(event_payload);
    assert(ret > 0);
    return ret;
}

int test_release_story(chronolog::Client &client, const std::string &chronicle_name, const std::string &story_name)
{
    random_sleep(); // TODO: (Kun) remove this when the hanging bug upon concurrent acquire is fixed
    int ret = client.ReleaseStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST);
    return ret;
}

int test_destroy_chronicle(chronolog::Client &client, const std::string &chronicle_name)
{
    int ret = client.DestroyChronicle(chronicle_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
    return ret;
}

int test_destroy_story(chronolog::Client &client, const std::string &chronicle_name, const std::string &story_name)
{
    int ret = client.DestroyStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
    return ret;
}

std::vector <std::string> parse_command_line(const std::string &line)
{
    std::vector <std::string> tokens;
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
        tokens.clear(); // Clear tokens to indicate an error
        return tokens;  // Return an empty list to signal failure
    }

    return tokens;
}

// function to extract and execute commands from command line input
void command_dispatcher(const std::string &command_line, std::unordered_map <std::string, std::function <void(
        std::vector <std::string> &)>> command_map)
{
    if(command_line.empty())
    {
        return;
    }

    std::vector <std::string> parsed_tokens = parse_command_line(command_line);

    if(parsed_tokens.empty())
    {
        return;
    }

    // Find the command in the map.
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

uint64_t get_event_timestamp(std::string &event_line)
{
    /*
     * Supported log files: syslog, auth.log, kern.log, ufw.log on Ubuntu
     * Expected format of log record:
     * Nov  5 14:36:49 ares-comp-01 systemd[1]: Started Time & Date Service.
     */
    size_t pos_first_space = event_line.find_first_of("0123456789") - 1;
    size_t pos_second_space = event_line.find_first_of(' ', pos_first_space + 1);
    size_t pos_third_space = event_line.find_first_of(' ', pos_second_space + 1);

    std::string timestamp_str = event_line.substr(0, pos_third_space);
    std::tm timeinfo{};
    auto now = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    std::tm*time_info_now = std::localtime(&current_time);
    // assume the log file is generated in the same year as the current time
    timeinfo.tm_year = time_info_now->tm_year;
    strptime(timestamp_str.c_str(), "%b %d %H:%M:%S", &timeinfo);
    uint64_t timestamp = timelocal(&timeinfo);
    return timestamp;
}

uint64_t get_bigbang_timestamp(std::ifstream &file)
{
    std::string line;
    std::getline(file, line);
    uint64_t bigbang_timestamp = get_event_timestamp(line);
    file.seekg(0, std::ios::beg);
    return bigbang_timestamp;
}

int main(int argc, char**argv)
{
    std::string argobots_conf_str = R"({"argobots" : {"abt_mem_max_num_stacks" : 8
                                                    , "abt_thread_stacksize" : 2097152}})";
    margo_set_environment(argobots_conf_str.c_str());

    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    std::string default_conf_file_path = "./default_conf.json";
    std::pair <std::string, workload_conf_args> cmd_args = cmd_arg_parse(argc, argv);
    std::string conf_file_path = cmd_args.first;
    workload_conf_args workload_args = cmd_args.second;
    if(conf_file_path.empty())
    {
        conf_file_path = default_conf_file_path;
    }
    chronolog::ClientPortalServiceConf portalConf("ofi+sockets", "127.0.0.1", 5555, 55);
    int result = chronolog::chrono_monitor::initialize("file", "chronoclient_logfile.txt", spdlog::level::debug, "ChronoClient", 102400, 3, spdlog::level::warn);

    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    chronolog::Client client(portalConf);
    chronolog::StoryHandle*story_handle;

    TimerWrapper connectTimer(workload_args.perf_test, "Connect");
    TimerWrapper createChronicleTimer(workload_args.perf_test, "CreateChronicle");
    TimerWrapper acquireStoryTimer(workload_args.perf_test, "AcquireStory");
    TimerWrapper writeEventTimer(workload_args.perf_test, "WriteEvent");
    TimerWrapper releaseStoryTimer(workload_args.perf_test, "ReleaseStory");
    TimerWrapper destroyStoryTimer(workload_args.perf_test, "DestroyStory");
    TimerWrapper destroyChronicleTimer(workload_args.perf_test, "DestroyChronicle");
    TimerWrapper disconnectTimer(workload_args.perf_test, "Disconnect");

    int ret_i;
    uint64_t ret_u;
    uint64_t event_payload_size_per_rank = 0;

    std::string client_id = gen_random(8);
//    std::cout << "Generated client id: " << client_id << std::endl;

    const std::string& server_protoc = portalConf.proto_conf();
    const std::string& server_ip = portalConf.ip();
    std::string server_port = std::to_string(portalConf.port());
    std::string server_provider_id = std::to_string(portalConf.provider_id());
    std::string server_address = server_protoc + "://" + server_ip + ":" + server_port + "@" + server_provider_id;

    std::string username = getpwuid(getuid())->pw_name;
    ret_i = connectTimer.timeBlock(&chronolog::Client::Connect, client);
    assert(ret_i == chronolog::CL_SUCCESS);

    std::cout << "Connected to server address: " << server_address << std::endl;
    std::string payload_str(MAX_EVENT_SIZE, 'a');

    double local_e2e_start = MPI_Wtime();
    if(workload_args.interactive)
    {
        // Interactive mode, accept commands from stdin
        std::cout << "Metadata operations: \n" << "\t-c <chronicle_name> , create a Chronicle <chronicle_name>\n"
                  << "\t-a -s <chronicle_name> <story_name>, acquire Story <story_name> in Chronicle <chronicle_name>\n"
                  << "\t-w <event_string>, write Event with <event_string> as payload\n"
                  << "\t-q -s <chronicle_name> <story_name>, release Story <story_name> in Chronicle <chronicle_name>\n"
                  << "\t-d -s <chronicle_name> <story_name>, destroy Story <story_name> in Chronicle <chronicle_name>\n"
                  << "\t-d -c <chronicle_name>, destroy Chronicle <chronicle_name>\n" << "\t-disconnect\n" << std::endl;

        std::unordered_map <std::string, std::function <void(std::vector <std::string> &)>> command_map = {{  "-c", [&](
std::vector <std::string> &command_subs)
        {
            assert(command_subs.size() == 2);
            std::string chronicle_name = command_subs[1];
            ret_i = test_create_chronicle(client, chronicle_name);
            if(ret_i == chronolog::CL_SUCCESS)
            {
                std::cout << "Chronicle created successfully: " << chronicle_name << std::endl;
            }
        }}
                                                                                                           , {"-a", [&](
                        std::vector <std::string> &command_subs)
                {
                    if(command_subs[1] == "-s")
                    {
                        assert(command_subs.size() == 4);
                        std::string chronicle_name = command_subs[2];
                        std::string story_name = command_subs[3];
                        story_handle = test_acquire_story(client, chronicle_name, story_name);
                        if(story_handle)
                        {
                            std::cout << "Story acquired successfully: " << story_name << " in Chronicle "
                                      << chronicle_name << std::endl;
                        }
                    }
                }}
                                                                                                           , {"-q", [&](
                        std::vector <std::string> &command_subs)
                {
                    if(command_subs[1] == "-s")
                    {
                        assert(command_subs.size() == 4);
                        std::string chronicle_name = command_subs[2];
                        std::string story_name = command_subs[3];
                        ret_i = test_release_story(client, chronicle_name, story_name);
                        if(ret_i == chronolog::CL_SUCCESS)
                        {
                            std::cout << "Story released successfully: " << story_name << " in Chronicle "
                                      << chronicle_name << std::endl;
                        }
                        else if(ret_i == chronolog::CL_ERR_NOT_EXIST)
                        {
                            std::cout << "Story does not exist: " << story_name << " in Chronicle "
                                      << chronicle_name << std::endl;
                        }
                    }
                }}
                                                                                                           , {"-w", [&](
                        std::vector <std::string> &command_subs)
                {
                    assert(command_subs.size() == 2);
                    std::string event_payload = command_subs[1];
                    ret_u = test_write_event(story_handle, event_payload);
                    if(ret_u > 0)
                    {
                        std::cout << "Event written successfully, payload length: " << event_payload.length()
                                  << std::endl;
                    }
                }}
                                                                                                           , {"-d", [&](
                        std::vector <std::string> &command_subs)
                {
                    if(command_subs[1] == "-c")
                    {
                        assert(command_subs.size() == 3);
                        std::string chronicle_name = command_subs[2];
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
                    }
                    else if(command_subs[1] == "-s")
                    {
                        assert(command_subs.size() == 4);
                        std::string chronicle_name = command_subs[2];
                        std::string story_name = command_subs[3];
                        ret_i = test_destroy_story(client, chronicle_name, story_name);
                        if(ret_i == chronolog::CL_SUCCESS)
                        {
                            std::cout << "Story destroyed successfully: " << story_name << " in Chronicle "
                                      << chronicle_name << std::endl;
                        }
                        else if(ret_i == chronolog::CL_ERR_ACQUIRED)
                        {
                            std::cout << "Story is still acquired, cannot destroy: " << story_name << " in Chronicle "
                                      << chronicle_name << std::endl;
                        }
                        else if(ret_i == chronolog::CL_ERR_NOT_EXIST)
                        {
                            std::cout << "Story does not exist: " << story_name << " in Chronicle "
                                      << chronicle_name << std::endl;
                        }
                    }
                }}};

        std::string command_line;
        while(true)
        {
            command_line.clear();
            std::getline(std::cin, command_line);
            command_line = trim_string(command_line);
            if(command_line == "-disconnect") break;
            command_dispatcher(command_line, command_map);
        }
    }
    else
    {
        // Non-interactive mode, execute workload via command line arguments
        std::random_device rand_device;
        std::mt19937 gen(rand_device());
        std::uniform_int_distribution char_dist(0, 255);
        double sigma = (double)(workload_args.max_event_size - workload_args.min_event_size) / 6;
        std::normal_distribution <double> size_dist((double)workload_args.ave_event_size, sigma);
        MPI_Barrier(MPI_COMM_WORLD);
        for(uint64_t i = 0; i < workload_args.chronicle_count; i++)
        {
            // create chronicle test
            std::string chronicle_name;
            if(workload_args.shared_story)
                chronicle_name = "chronicle_" + std::to_string(i);
            else
                chronicle_name = "chronicle_" + std::to_string(rank) + "_" + std::to_string(i);
            ret_i = createChronicleTimer.timeBlock(test_create_chronicle, client, chronicle_name);
            if(workload_args.barrier)
                MPI_Barrier(MPI_COMM_WORLD);

            uint64_t story_count_per_chronicle = workload_args.story_count / workload_args.chronicle_count;
            for(uint64_t j = 0; j < story_count_per_chronicle; j++)
            {
                // acquire story test
                std::string story_name;
                if(workload_args.shared_story)
                    story_name = "story_" + std::to_string(j);
                else
                    story_name = "story_" + std::to_string(rank) + "_" + std::to_string(j);
                story_handle = acquireStoryTimer.timeBlock(test_acquire_story, client, chronicle_name, story_name);
                if(workload_args.barrier)
                    MPI_Barrier(MPI_COMM_WORLD);

                // write event test
                std::string event_payload;
                writeEventTimer.timeBlock([&]()
                                {
                                    uint64_t event_count_per_story =
                                            workload_args.event_count / workload_args.story_count;
                                    for(uint64_t k = 0; k < event_count_per_story; k++)
                                    {
                                        if(workload_args.event_payload_file.empty())
                                        {
                                            // randomly generate events size if range is specified
                                            writeEventTimer.pauseTimer();

                                            uint64_t event_size;
                                            if(workload_args.ave_event_size == workload_args.min_event_size &&
                                               workload_args.ave_event_size == workload_args.max_event_size)
                                            {
                                                event_size = workload_args.ave_event_size;
                                            }
                                            else
                                            {
                                                event_size = (unsigned long)std::min(std::max(size_dist(gen),
                                                                                             (double)workload_args.min_event_size * 1.0),
                                                        (double)workload_args.max_event_size * 1.0);
                                            }
                                            event_payload = payload_str.substr(0, event_size);
                                            event_payload_size_per_rank += event_size;
                                            writeEventTimer.resumeTimer();
                                            ret_u = test_write_event(story_handle, event_payload);
                                            if(workload_args.barrier)
                                                MPI_Barrier(MPI_COMM_WORLD);

                                            if(workload_args.event_interval > 0)
                                                usleep(workload_args.event_interval);
                                        }
                                        else
                                        {
                                            // read event payload from input file line by line
                                            std::ifstream input_file(workload_args.event_payload_file);

                                            // check if the file opened successfully
                                            if(input_file.is_open())
                                            {
                                                writeEventTimer.pauseTimer();
                                                uint64_t bigbang_timestamp = get_bigbang_timestamp(input_file);
                                                uint64_t last_event_timestamp = bigbang_timestamp;
                                                uint64_t event_timestamp;
                                                struct timespec sleep_ts{};
                                                while(std::getline(input_file, event_payload))
                                                {
                                                    if(event_payload.empty()) continue;
                                                    event_timestamp = get_event_timestamp(event_payload);
                                                    if(event_timestamp < last_event_timestamp)
                                                    {
                                                        LOG_INFO(
                                                                "An Out-of-Order event is found, sleeping for 1 second ...");
                                                        sleep_ts.tv_sec = 1;
                                                        sleep_ts.tv_nsec = 0;
                                                    }
                                                    else
                                                    {
                                                        sleep_ts.tv_sec =
                                                                (long)(event_timestamp - last_event_timestamp) /
                                                                1000000000;
                                                        sleep_ts.tv_nsec =
                                                                (long)(event_timestamp - last_event_timestamp) %
                                                                1000000000;
                                                    }
                                                    // TODO: (Kun) work around on failure when daytime changes
                                                    if(sleep_ts.tv_sec > 3600) sleep_ts.tv_sec = 0;
                                                    LOG_DEBUG(
                                                            "Sleeping for {}.{} seconds to emulate interval between events ..."
                                                            , sleep_ts.tv_sec, sleep_ts.tv_nsec);
                                                    nanosleep(&sleep_ts, nullptr);
                                                    last_event_timestamp = event_timestamp;
                                                    event_payload_size_per_rank += event_payload.size();
                                                    writeEventTimer.resumeTimer();
                                                    ret_u = test_write_event(story_handle, event_payload);
                                                    if(workload_args.barrier)
                                                        MPI_Barrier(MPI_COMM_WORLD);
                                                }

                                                input_file.close();
                                            }
                                            else
                                            {
                                                std::cout << "Unable to open the file";
                                            }
                                        }
                                    }
                                });

                // release story test
                ret_i = releaseStoryTimer.timeBlock(test_release_story, client, chronicle_name, story_name);
                if(workload_args.barrier)
                    MPI_Barrier(MPI_COMM_WORLD);

                // destroy story test
                ret_i = destroyStoryTimer.timeBlock(test_destroy_story, client, chronicle_name, story_name);
                if(workload_args.barrier)
                    MPI_Barrier(MPI_COMM_WORLD);
            }

            // destroy chronicle test
            ret_i = destroyChronicleTimer.timeBlock(test_destroy_chronicle, client, chronicle_name);
            if(workload_args.barrier)
                MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    double local_e2e_end = MPI_Wtime();

    ret_i = disconnectTimer.timeBlock(&chronolog::Client::Disconnect, client);
    assert(ret_i == chronolog::CL_SUCCESS);
    if(workload_args.barrier)
        MPI_Barrier(MPI_COMM_WORLD);

    if(workload_args.perf_test && !workload_args.interactive)
    {
        double local_e2e_duration = local_e2e_end - local_e2e_start;
        double global_e2e_start, global_e2e_end, global_e2e_duration_ave, global_e2e_duration;
        MPI_Reduce(&local_e2e_start, &global_e2e_start, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
        MPI_Reduce(&local_e2e_end, &global_e2e_end, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        global_e2e_duration = global_e2e_end - global_e2e_start;
        MPI_Reduce(&local_e2e_duration, &global_e2e_duration_ave, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

        uint64_t total_event_payload_size = 0;
        MPI_Reduce(&event_payload_size_per_rank, &total_event_payload_size, 1, MPI_UINT64_T, MPI_SUM, 0
                   , MPI_COMM_WORLD);
        if(rank == 0)
        {
            std::cout << "======================================" << std::endl;
            std::cout << "======== Performance results: ========" << std::endl;
            std::cout << "======================================" << std::endl;
            global_e2e_duration_ave /= size;
            std::cout << "Total payload written: " << total_event_payload_size << std::endl;
            if(workload_args.barrier)
            {
                std::cout << "Connect throughput: " << (double)size / connectTimer.getDuration() << " op/s"
                          << std::endl;
                std::cout << "CreateChronicle throughput: "
                          << (double)workload_args.chronicle_count * size / createChronicleTimer.getDuration()
                          << " op/s" << std::endl;
                std::cout << "AcquireStory throughput: "
                          << (double)workload_args.story_count * size / acquireStoryTimer.getDuration() << " op/s"
                          << std::endl;
                std::cout << "ReleaseStory throughput: "
                          << (double)workload_args.story_count * size / releaseStoryTimer.getDuration() << " op/s"
                          << std::endl;
                std::cout << "DestroyStory throughput: "
                          << (double)workload_args.story_count * size / destroyStoryTimer.getDuration() << " op/s"
                          << std::endl;
                std::cout << "DestroyChronicle throughput: "
                          << (double)workload_args.chronicle_count * size / destroyChronicleTimer.getDuration()
                          << " op/s" << std::endl;
                std::cout << "Disconnect throughput: " << (double)size / disconnectTimer.getDuration() << " op/s"
                          << std::endl;
                std::cout << "End-to-end bandwidth: " << (double)total_event_payload_size / global_e2e_duration / 1e6
                          << " MB/s" << std::endl;
                std::cout << "Data-access bandwidth: "
                          << (double)total_event_payload_size / writeEventTimer.getDuration() / 1e6 << " "
                                                                                                       "MB/s"
                          << std::endl;
            }
            else
            {
                std::cout << "Connect throughput: " << (double)size / connectTimer.getDurationAve() << " op/s"
                          << std::endl;
                std::cout << "CreateChronicle throughput: "
                          << (double)workload_args.chronicle_count * size / createChronicleTimer.getDurationAve()
                          << " op/s" << std::endl;
                std::cout << "AcquireStory throughput: "
                          << (double)workload_args.story_count * size / acquireStoryTimer.getDurationAve() << " op/s"
                          << std::endl;
                std::cout << "ReleaseStory throughput: "
                          << (double)workload_args.story_count * size / releaseStoryTimer.getDurationAve() << " op/s"
                          << std::endl;
                std::cout << "DestroyStory throughput: "
                          << (double)workload_args.story_count * size / destroyStoryTimer.getDurationAve() << " op/s"
                          << std::endl;
                std::cout << "DestroyChronicle throughput: "
                          << (double)workload_args.chronicle_count / destroyChronicleTimer.getDurationAve() << " op/s"
                          << std::endl;
                std::cout << "Disconnect throughput: " << (double)size / disconnectTimer.getDurationAve() << " op/s"
                          << std::endl;
                std::cout << "End-to-end bandwidth: "
                          << (double)total_event_payload_size / global_e2e_duration_ave / 1e6 << " MB/s" << std::endl;
                std::cout << "Data-access bandwidth: "
                          << (double)total_event_payload_size / writeEventTimer.getDurationAve() / 1e6 << " MB/s"
                          << std::endl;
            }
        }
    }

    MPI_Finalize();

    return 0;
}
