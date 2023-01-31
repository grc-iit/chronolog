//
// Created by kfeng on 3/30/22.
//

#ifndef CHRONOLOG_DATA_STRUCTURES_H
#define CHRONOLOG_DATA_STRUCTURES_H

#include <string>
#include <cstring>
#include <vector>
#include <cstdint>
#include <chrono>

typedef struct ChronoLogCharStruct {
private:
    char value[256];
    void Set(char* data_, size_t size) {
        snprintf(this->value, size+1, "%s", data_);
    }
    void Set(const std::string& data_) {
        snprintf(this->value, data_.length()+1, "%s", data_.c_str());
    }
public:
    ChronoLogCharStruct() {}

    ChronoLogCharStruct(const ChronoLogCharStruct &other) : ChronoLogCharStruct(other.value) {} /* copy constructor*/
    ChronoLogCharStruct(ChronoLogCharStruct &&other) : ChronoLogCharStruct(other.value) {} /* move constructor*/

    ChronoLogCharStruct(const char *data_) {
        snprintf(this->value, strlen(data_) + 1, "%s", data_);
    }

    ChronoLogCharStruct(std::string data_) : ChronoLogCharStruct(data_.c_str()) {}

    ChronoLogCharStruct(char *data_, size_t size) {
        snprintf(this->value, size, "%s", data_);
    }

    const char *c_str() const {
        return value;
    }

    std::string string() const {
        return std::string(value);
    }

    char* data() {
        return value;
    }

    size_t size() const {
        return strlen(value);
    }
    /**
   * Operators
   */
    ChronoLogCharStruct &operator=(const ChronoLogCharStruct &other) {
        strcpy(value,other.c_str());
        return *this;
    }
    /* equal operator for comparing two Chars. */
    bool operator==(const ChronoLogCharStruct &o) const {
        return strcmp(value, o.value) == 0;
    }
    ChronoLogCharStruct operator+(const ChronoLogCharStruct& o){
        std::string added=std::string(this->c_str())+std::string(o.c_str());
        return ChronoLogCharStruct(added);
    }
    ChronoLogCharStruct operator+(std::string &o)
    {
        std::string added=std::string(this->c_str())+o;
        return ChronoLogCharStruct(added);
    }
    ChronoLogCharStruct& operator+=(const ChronoLogCharStruct& rhs){
        std::string added=std::string(this->c_str())+std::string(rhs.c_str());
        Set(added);
        return *this;
    }
    bool operator>(const ChronoLogCharStruct &o) const {
        return  strcmp(this->value,o.c_str()) > 0;
    }
    bool operator>=(const ChronoLogCharStruct &o) const {
        return strcmp(this->value,o.c_str()) >= 0;
    }
    bool operator<(const ChronoLogCharStruct &o) const {
        return strcmp(this->value,o.c_str()) < 0;
    }
    bool operator<=(const ChronoLogCharStruct &o) const {
        return strcmp(this->value,o.c_str()) <= 0;
    }

} ChronoLogCharStruct;

namespace std {
    template<>
    struct hash<ChronoLogCharStruct> {
        size_t operator()(const ChronoLogCharStruct &k) const {
            std::string val(k.c_str());
            return std::hash<std::string>()(val);
        }
    };
}

typedef struct GetClockResponse_ {
    uint64_t t_arrival;
    uint64_t t_departure;
    double drift_rate;

    template<typename SerArchiveT>
    void serialize(SerArchiveT &serT) {
        serT & t_arrival;
        serT & t_departure;
        serT & drift_rate;
    }
} GetClockResponse;

#endif //CHRONOLOG_DATA_STRUCTURES_H
