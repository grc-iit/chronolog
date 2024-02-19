//
// Created by kfeng on 7/25/23.
//

#ifndef CHRONOLOG_CMD_ARG_PARSE_H
#define CHRONOLOG_CMD_ARG_PARSE_H

#include <getopt.h>
#include <string>
#include <iostream>

std::string parse_conf_path_arg(int argc, char**argv)
{
    int opt;
    char*config_file = nullptr;

    // Define the long options and their corresponding short options
    struct option long_options[] = {{  "config", required_argument, 0, 'c'}
                                    , {0       , 0                , 0, 0} // Terminate the options array
    };

    // Parse the command-line options
    while((opt = getopt_long(argc, argv, "c:", long_options, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                config_file = optarg;
                break;
            case '?':
                // Invalid option or missing argument
                std::cout
                        << "[cmd_arg_parse] Invalid usage: Please provide a configuration file path using -c or --config option."
                        << std::endl;
                exit(EXIT_FAILURE);
            default:
                // Unknown option
                std::cerr << "[cmd_arg_parse] Encountered an unknown option: " << static_cast<char>(opt) << std::endl;
                exit(EXIT_FAILURE);
        }
    }

    // Check if the config file option is provided
    if(config_file)
    {
        std::cout << "[cmd_arg_parse] Configuration file path provided: " << config_file << std::endl;
        return {std::string(config_file)};
    }
    else
    {
        std::cerr << "[cmd_arg_parse] No configuration file path provided" << std::endl;
        return "";
    }
}

#endif //CHRONOLOG_CMD_ARG_PARSE_H
