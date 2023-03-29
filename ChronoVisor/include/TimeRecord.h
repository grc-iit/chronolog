//
// Created by kfeng on 10/25/21.
//

#ifndef CHRONOLOG_TIMERECORD_H
#define CHRONOLOG_TIMERECORD_H

#include <string>
#include <iomanip>
#include <sstream>
#include <cassert>
#include "ClocksourceManager.h"

/**
 * Class of each record in TimeDB, which might not be the same as TimeInfo in message serde
 */
class TimeRecord {
public:
    TimeRecord() : timestamp_(0), drift_rate_(0), ref_interval_(-1), clocksource_(nullptr) {}

    explicit TimeRecord(Clocksource *clocksource) : timestamp_(0), drift_rate_(0), ref_interval_(-1) {
        clocksource_ = clocksource;
    }

    TimeRecord(Clocksource *clocksource, uint64_t timestamp, double ref_interval)
    : timestamp_(timestamp), ref_interval_(ref_interval), clocksource_(clocksource) {}

    TimeRecord& operator=(const TimeRecord& other) {
        if (this == &other)
            return *this;

        this->timestamp_ = other.timestamp_;
        this->drift_rate_ = other.drift_rate_;

        return *this;
    }

    bool operator==(const TimeRecord& other) const {
        return (this->timestamp_ == other.timestamp_) && (this->drift_rate_ == other.drift_rate_);
    }

    bool operator!=(const TimeRecord& other) const {
        return (this->timestamp_ != other.timestamp_) || (this->drift_rate_ != other.drift_rate_);
    }

    std::string toString() const {
        std::string str = "timestamp: ";
        str.append(std::to_string(timestamp_));
        str.append(", drift rate: ");
        std::ostringstream str_sci;
        str_sci << std::setprecision(6);
        str_sci << drift_rate_;
        str.append(str_sci.str());

        return str;
    }

    Clocksource *getClocksource() { return clocksource_; }
    uint64_t getTimestamp() const { return timestamp_; }
    double getDriftRate() const { return drift_rate_; }

    void setClocksource(Clocksource *clocksource) { clocksource_ = clocksource; }
    void setUpdateInterval(double update_interval) { ref_interval_ = update_interval; }
    void setTimestamp(uint64_t timestamp) { timestamp_ = timestamp; }
    void setDriftRate(double drift_rate) { drift_rate_ = drift_rate; }

    /**
     * Update timestamp and drift rate
     * @param last_timestamp timestamp at last time
     */
    void updateTimeRecord(uint64_t last_timestamp) {
        timestamp_ = getCurTimestamp();
        drift_rate_ = calDriftRate(last_timestamp);
    }

    /**
     * Update timestamp every interval
     */
    void updateTimestamp() { timestamp_ = getCurTimestamp(); }

    /**
     * Calculate drift rate using parameter last_timestamp and stored ref_interval_
     * @param last_timestamp
     * @return
     */
    double calDriftRate(uint64_t last_timestamp) {
        assert(ref_interval_ != -1);
        uint64_t cur = getCurTimestamp();
        return (double) (cur - last_timestamp) / ref_interval_ / 1000000000.0 - 1;
    }

public:
    uint64_t    timestamp_{};          ///< updated every interval, key to fetch
    double      drift_rate_{};         ///< drift rate calculated as diff(timestamp1, timestamp2)/interval
    double      ref_interval_{};       ///< update interval in second

private:
    /**
     * Get system timestamp
     * @return current system timestamp
     */
    uint64_t getCurTimestamp() {
        assert(clocksource_ != nullptr);
        return clocksource_->getTimestamp();
    }

    Clocksource *clocksource_;   ///< clocksource (clock_gettime, std::high_precision_clock::now(), or TSC)
};

#endif //CHRONOLOG_TIMERECORD_H
