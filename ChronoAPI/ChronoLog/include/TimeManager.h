//
// Created by kfeng on 2/14/22.
//

#ifndef CHRONOLOG_TIMEMANAGER_H
#define CHRONOLOG_TIMEMANAGER_H

#include <atomic>
#include "ClocksourceManager.h"
#include "TimeRecord.h"

#define TIME_DB_INIT_WAIT 1
#define TIME_DB_UPDATE_INTERVAL 3

class TimeManager
{
public:
    TimeManager()
    {
        refTimestampUpdateInterval_ = TIME_DB_UPDATE_INTERVAL;
        init(refTimestampUpdateInterval_);
        Logger::getLogger()->info("[TimeManager] Initialized with default update interval: {}"
                                  , refTimestampUpdateInterval_);
    }

    explicit TimeManager(double updateInterval)
    {
        refTimestampUpdateInterval_ = updateInterval;
        init(refTimestampUpdateInterval_);
        Logger::getLogger()->info("[TimeManager] Initialized with update interval: {}", updateInterval);
    }

    explicit TimeManager(double updateInterval, ClocksourceType clocksourceType)
    {
        refTimestampUpdateInterval_ = updateInterval;
        init(refTimestampUpdateInterval_, clocksourceType);
        Logger::getLogger()->info("[TimeManager] Initialized with update interval: {} and clocksource type: {}"
                                  , updateInterval, static_cast<int>(clocksourceType));
    }

    void setClocksourceType(ClocksourceType clocksourceType)
    {
        clocksourceType_ = clocksourceType;
    }

    std::unique_ptr <TimeRecord> genPeriodicTimeRecord()
    {
        std::unique_ptr <TimeRecord> record = std::make_unique <TimeRecord>(clocksource_);
        if(firstTimeRecord_)
        {
            // very first time of generating a TimeRecord for big bang, sleep 1s and calculate drift rate
            record->updateTimestamp();
            uint64_t lastTimestamp = record->getTimestamp();
            record->setUpdateInterval(TIME_DB_INIT_WAIT);
            usleep(TIME_DB_INIT_WAIT * 1e6);
            record->updateTimeRecord(lastTimestamp);
            latestDriftRate_ = record->getDriftRate();
        }
        else
        {
            record->setUpdateInterval(refTimestampUpdateInterval_);
            record->setDriftRate(latestDriftRate_);
            if(lastTimestamp_ == 0)
            {
                // first interval TimeRecord
                lastTimestamp_ = clocksource_->getTimestamp();
            }
            else
            {
                uint64_t curTimestamp_ = clocksource_->getTimestamp();
                latestDriftRate_ =
                        (double)(curTimestamp_ - lastTimestamp_) / refTimestampUpdateInterval_ / 1000000000.0 - 1;
                lastTimestamp_ = curTimestamp_;
            }
        }
        firstTimeRecord_ = false;
        record->updateTimestamp();
        return record;
    }

    std::unique_ptr <TimeRecord> genOnDemandTimeRecord()
    {
        std::unique_ptr <TimeRecord> record = std::make_unique <TimeRecord>(clocksource_);
        record->updateTimestamp();
        record->setDriftRate(latestDriftRate_);
        return record;
    }

public:
    Clocksource*clocksource_{};               ///< clocksource (clock_gettime, std::high_precision_clock::now(), or TSC)
    double refTimestampUpdateInterval_;      ///< how often reference timestamp is updated for drift rate calculation
    uint64_t lastTimestamp_{};
    std::atomic <double> latestDriftRate_{};
    bool firstTimeRecord_{};
    ClocksourceType clocksourceType_{};

private:
    void init(double updateInterval, ClocksourceType clocksourceType = ClocksourceType::C_STYLE)
    {
        firstTimeRecord_ = true;
        refTimestampUpdateInterval_ = updateInterval;
        clocksourceType_ = clocksourceType;
        ClocksourceManager*manager = ClocksourceManager::getInstance();
        manager->setClocksourceType(clocksourceType_);
        clocksource_ = manager->getClocksource();
    }
};


#endif //CHRONOLOG_TIMEMANAGER_H
