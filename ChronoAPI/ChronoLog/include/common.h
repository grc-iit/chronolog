//
// Created by kfeng on 2/14/22.
//

#ifndef CHRONOLOG_COMMON_H
#define CHRONOLOG_COMMON_H

#include <random>

std::random_device rd;
std::seed_seq ssq{rd()};
std::default_random_engine mt{rd()};
std::uniform_int_distribution <int> dist(0, INT32_MAX);

std::string gen_random(const int len)
{
    static const char alphanum[] = "0123456789"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz";
    std::string tmp_s;
    tmp_s.reserve(len);

    for(int i = 0; i < len; ++i)
    {
        tmp_s += alphanum[dist(mt) % (sizeof(alphanum) - 1)];
    }

    return tmp_s;
}

#endif //CHRONOLOG_COMMON_H
