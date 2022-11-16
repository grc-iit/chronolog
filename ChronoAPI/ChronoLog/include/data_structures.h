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

typedef struct CharStruct {
private:
    char value[256];
    void Set(char* data_, size_t size) {
        snprintf(this->value, size+1, "%s", data_);
    }
    void Set(const std::string& data_) {
        snprintf(this->value, data_.length()+1, "%s", data_.c_str());
    }
public:
    CharStruct() {}

    CharStruct(const CharStruct &other) : CharStruct(other.value) {} /* copy constructor*/
    CharStruct(CharStruct &&other) : CharStruct(other.value) {} /* move constructor*/

    CharStruct(const char *data_) {
        snprintf(this->value, strlen(data_) + 1, "%s", data_);
    }

    CharStruct(std::string data_) : CharStruct(data_.c_str()) {}

    CharStruct(char *data_, size_t size) {
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
    CharStruct &operator=(const CharStruct &other) {
        strcpy(value,other.c_str());
        return *this;
    }
    /* equal operator for comparing two Chars. */
    bool operator==(const CharStruct &o) const {
        return strcmp(value, o.value) == 0;
    }
    CharStruct operator+(const CharStruct& o){
        std::string added=std::string(this->c_str())+std::string(o.c_str());
        return CharStruct(added);
    }
    CharStruct operator+(std::string &o)
    {
        std::string added=std::string(this->c_str())+o;
        return CharStruct(added);
    }
    CharStruct& operator+=(const CharStruct& rhs){
        std::string added=std::string(this->c_str())+std::string(rhs.c_str());
        Set(added);
        return *this;
    }
    bool operator>(const CharStruct &o) const {
        return  strcmp(this->value,o.c_str()) > 0;
    }
    bool operator>=(const CharStruct &o) const {
        return strcmp(this->value,o.c_str()) >= 0;
    }
    bool operator<(const CharStruct &o) const {
        return strcmp(this->value,o.c_str()) < 0;
    }
    bool operator<=(const CharStruct &o) const {
        return strcmp(this->value,o.c_str()) <= 0;
    }

} CharStruct;

namespace std {
    template<>
    struct hash<CharStruct> {
        size_t operator()(const CharStruct &k) const {
            std::string val(k.c_str());
            return std::hash<std::string>()(val);
        }
    };
}

#endif //CHRONOLOG_DATA_STRUCTURES_H
