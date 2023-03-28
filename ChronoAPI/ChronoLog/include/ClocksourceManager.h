//
// Created by kfeng on 2/8/22.
//

#ifndef CHRONOLOG_CLOCKSOURCEMANAGER_H
#define CHRONOLOG_CLOCKSOURCEMANAGER_H

#include <chrono>
#include <cstdint>
#include <ctime>
#include <typeinfo>
#include <unistd.h>
#include <enum.h>
#include <log.h>
#include <climits>
#include <rpc.h>
#include <algorithm>
#include <errcode.h>
#include <mutex>

class ClockSourceCStyle
{
   public:
	 ClockSourceCStyle(){}
	 ~ClockSourceCStyle(){}
	uint64_t getTimeStamp()
	{
		struct timespec t{};
		clock_gettime(CLOCK_TAI,&t);
		return t.tv_nsec;
	}
};

class ClockSourceCPPStyle
{
   public:
	ClockSourceCPPStyle() {}
	~ClockSourceCPPStyle(){}
	uint64_t getTimeStamp()
	{
	    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	    return t;
	}
};

template<class ClockSource>
class ClocksourceManager {
public:
	ClocksourceManager(std::string &unit_name)
	{
	  unit = 1;
	  if(unit_name.compare("nanoseconds")==0) unit = 1;
	  else if(unit_name.compare("microseconds")==0) unit = 1000;
	  else if(unit_name.compare("milliseconds")==0) unit = 1000000;
	  else if(unit_name.compare("seconds")==0) unit = 1000000000;
	  offset = 0;
	  pos_neg = false;
	  func_prefix = "ChronoLogThallium";
	}
	~ClocksourceManager()
	{}
	uint64_t TimeStamp()
	{
		if(!pos_neg)
		return clocksource_->getTimeStamp()/unit+offset;
		else return clocksource_->getTimeStamp()/unit-offset;
	}
	void set_rpc(std::shared_ptr<ChronoLogRPC> &r)
	{
		rpc = r;
	}
	uint64_t LocalGetTS(std::string &client_id)
	{
	     uint64_t t = TimeStamp();

	     return t;

	}

        void bind_functions()
	{
		std::function<void(const tl::request &,
				   std::string &
                                   )> timestampFunc(
                        [this](auto && PH1,
			       auto && PH2
				) {
                            ThalliumLocalGetTS(std::forward<decltype(PH1)>(PH1),
					    std::forward<decltype(PH2)>(PH2));
			    });

		rpc->bind("ChronoLogThalliumGetTS", timestampFunc);
	}


	CHRONOLOG_THALLIUM_DEFINE(LocalGetTS,(client_id),std::string& client_id)

	void set_offset(uint64_t &o, bool &b)
	{
		offset = o;
		pos_neg = b;
	}
        
	uint64_t get_offset()
	{
	     return offset;
        }	     
private:
    std::shared_ptr<ChronoLogRPC> rpc;
    ClockSource *clocksource_;
    uint64_t unit;
    uint64_t offset;
    bool pos_neg;
    std::string func_prefix;
};

#endif //CHRONOLOG_CLOCKSOURCEMANAGER_H
