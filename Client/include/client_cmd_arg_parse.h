#ifndef CHRONOLOG_CLIENT_CMD_ARG_PARSE_H
#define CHRONOLOG_CLIENT_CMD_ARG_PARSE_H

#include <getopt.h>
#include <string>
#include <iostream>

namespace chronolog {

inline std::string parse_conf_path_arg(int argc, char** argv) {
    int opt;
    char* config_file = nullptr;

    struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;
            case '?':
                std::cerr << "[CLIENT_CMD_ARG_PARSE] Invalid usage: Use -c or --config <path> to specify config file." << std::endl;
                std::exit(EXIT_FAILURE);
            default:
                std::cerr << "[CLIENT_CMD_ARG_PARSE] Unknown option: " << static_cast<char>(opt) << std::endl;
                std::exit(EXIT_FAILURE);
        }
    }

    if (config_file) {
        std::cout << "[CLIENT_CMD_ARG_PARSE] Configuration file path: " << config_file << std::endl;
        return std::string(config_file);
    } else {
        std::cerr << "[CLIENT_CMD_ARG_PARSE] No configuration file provided." << std::endl;
        return "";
    }
}

} // namespace chronolog

#endif // CHRONOLOG_CLIENT_CMD_ARG_PARSE_H