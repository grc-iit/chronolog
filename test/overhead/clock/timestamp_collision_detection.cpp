//
// Created by kfeng on 3/30/23.
//

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <regex>
#include <filesystem>

int main() {
    // Define the path to the directory containing the files
    std::string dir_path = "/home/kfeng/CLionProjects/ChronoLog_dev/decouple_conf_manager/ChronoLog/cmake-build-release/test/overhead/clock/";

    // Define the regular expression to match the file names
    std::regex file_regex("clock_gettime_thread(\\d+)");

    // Define an unordered map to store the count of each number
    std::unordered_map<uint64_t, uint64_t> count_map;

    // Loop over all files in the directory
    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        // Check if the file name matches the regular expression
        std::string filename = entry.path().filename().string();
        if (!std::regex_match(filename, file_regex)) {
            continue;
        }

        // Open the file and read its contents
        std::cout << "reading from file " << entry.path().string() << std::endl;
        std::ifstream file(entry.path());
        uint64_t num;
        while (file >> num) {
            // Increment the count of the number in the map
            count_map[num]++;
        }
    }
    std::cout << count_map.size() << " unique numbers are read" << std::endl;

    // Open a file to write the duplicate numbers and their counts
//    std::ofstream out_file("duplicates.txt", std::ios::trunc);

    // Loop over the entries in the map and write the duplicates to the output file
//    for (const auto& entry : count_map) {
//        if (entry.second > 1) {
//            out_file << entry.first << " " << entry.second << std::endl;
//        }
//    }

    // Close the output file
//    out_file.close();

    return 0;
}
