//
// Created by kfeng on 3/6/22.
//

#ifndef CHRONOLOG_ARCHIVE_H
#define CHRONOLOG_ARCHIVE_H

#include <string>
#include <city.h>

class Archive {
public:
    Archive() {};

    const std::string &getName() const { return name_; }
    const uint64_t &getAid() const { return aid_; }
    const uint64_t &getCid() const { return cid_; }

    void setName(const std::string &name) { name_ = name; }
    void setAid(uint64_t aid) { aid_ = aid; }
    void setCid(uint64_t cid) { cid_ = cid; }

    friend std::ostream& operator<<(std::ostream& os, const Archive& archive);

private:
    std::string name_;
    uint64_t aid_{};
    uint64_t cid_{};
};

inline std::ostream& operator<<(std::ostream& os, const Archive& archive) {
    os << "name: " << archive.name_ << ", "
       << "aid: " << archive.aid_ << ", "
       << "cid: " << archive.cid_;
    return os;
}

#endif //CHRONOLOG_ARCHIVE_H
