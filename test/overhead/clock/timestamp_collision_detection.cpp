//
// Created by kfeng on 3/30/23.
//

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <regex>
#include <filesystem>
#include "chrono_monitor.h"

int main(int argc, char*argv[])
{
    // Define the path to the directory containing the files
    std::string dir_path = "./";
    if(argc > 1) dir_path = argv[1];

    // Define the regular expression to match the file names
    std::regex file_regex("clock_gettime_thread.*");

    // Define an unordered map to store the count of each number
    std::unordered_map <uint64_t, uint64_t> count_map;

    // Loop over all files in the directory
    uint64_t total_count = 0;
    for(const auto &entry: std::filesystem::directory_iterator(dir_path))
    {
        // Check if the file name matches the regular expression
        std::string filename = entry.path().filename().string();
        if(!std::regex_match(filename, file_regex))
        {
            continue;
        }

        // Open the file and read its contents
        LOG_INFO("Reading from file {}", entry.path().string());
        /*std::cout << "reading from file " << entry.path().string() << std::endl;*/

        std::ifstream file(entry.path());
        uint64_t num;
        while(file >> num)
        {
            // Increment the count of the number in the map
            count_map[num]++;
            total_count++;
        }
    }
    LOG_INFO("{} unique numbers are read", count_map.size());
    /*std::cout << count_map.size() << " unique numbers are read" << std::endl;*/
    double collision_ratio = 1 - count_map.size() * 1.0 / total_count;
    LOG_INFO("Collision ratio: {}%", collision_ratio * 100);
    /*std::cout << "Collision ratio: " << collision_ratio * 100 << "%" << std::endl;*/


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
