//
// Created by kfeng on 3/7/22.
//

#ifndef CHRONOLOG_EVENT_H
#define CHRONOLOG_EVENT_H

#include <atomic>
#include <cstdint>

class Event
{
public:
    Event(): data_(nullptr)
    {};

    const uint64_t &getSid() const
    { return sid_; }

    const uint64_t &getTimestamp() const
    { return timestamp_; }

    void*getData() const
    { return data_; }

    size_t getSize() const
    { return size_; }

    void setSid(const uint64_t &sid)
    { sid_ = sid; }

    void setTimestamp(const uint64_t &timestamp)
    { timestamp_ = timestamp; }

    void setData(void*data)
    { Event::data_ = data; }

    void setSize(size_t size)
    { size_ = size; }

private:
    uint64_t sid_{};
    uint64_t timestamp_{};
    void*data_;
    size_t size_{};
};

#endif //CHRONOLOG_EVENT_H
