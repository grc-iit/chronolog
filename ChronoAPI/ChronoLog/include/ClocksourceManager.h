//
// Created by kfeng on 2/8/22.
//

#ifndef CHRONOLOG_CLOCKSOURCEMANAGER_H
#define CHRONOLOG_CLOCKSOURCEMANAGER_H

#include <chrono>
#include <cstdint>
#include <ctime>
#include <unistd.h>
#include <emmintrin.h>
#include <log.h>

#define   lfence()  _mm_lfence()
#define   mfence()  _mm_mfence()

enum ClocksourceType {
    C_STYLE = 0,
    CPP_STYLE = 1,
    TSC = 2,
    UNKNOWN = 3
};

class Clocksource {
public:
    static Clocksource *Create(ClocksourceType type);
    virtual ~Clocksource() = default;;
    /**
     * @name Get timestamp
     */
    ///@{
    virtual uint64_t getTimestamp() = 0;
    ///@}
};

class ClocksourceCStyle : public Clocksource {
public:
    uint64_t getTimestamp() override {
        struct timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (t.tv_sec * (uint64_t) 1e9 + t.tv_nsec);
    }
};

class ClocksourceTSC : public Clocksource {
public:
    uint64_t getTimestamp() override {
        unsigned int proc_id;
        uint64_t  t = __builtin_ia32_rdtscp(&proc_id);
        lfence();
        return t;
    }
};
class ClocksourceCPPStyle : public Clocksource {
public:
    uint64_t getTimestamp() override {
        using namespace std::chrono;
        using clock = steady_clock;
        clock::time_point t = clock::now();
        return (t.time_since_epoch().count());
    }
};

class ClocksourceManager {
private:
    ClocksourceManager() : clocksource_(nullptr), clocksourceType_(ClocksourceType::UNKNOWN) {}

public:
    ~ClocksourceManager() {
        if (clocksource_) {
            delete[] clocksource_;
            clocksource_ = nullptr;
        }
        if (clocksourceManager_) {
            delete[] clocksourceManager_;
            clocksourceManager_ = nullptr;
        }
    }

    static ClocksourceManager *getInstance() {
        if (!clocksourceManager_) {
            clocksourceManager_ = new ClocksourceManager();
        }
        return clocksourceManager_;
    }

    void setClocksourceType(ClocksourceType type) {
        this->clocksourceType_ = type;
        LOGD("clocksource is set to (0 for C, 1 for C++, 2 for TSC): %d", clocksourceType_);
    }

    Clocksource *getClocksource() {
        this->clocksource_ = Clocksource::Create(this->clocksourceType_);
        return this->clocksource_;
    }
private:
    static ClocksourceManager *clocksourceManager_;
    Clocksource *clocksource_;
    ClocksourceType clocksourceType_;
};

#endif //CHRONOLOG_CLOCKSOURCEMANAGER_H
