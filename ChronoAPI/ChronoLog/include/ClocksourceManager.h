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
	ClocksourceManager()
	{
	  unit = 1000;
	  offset = 0;
	  num_procs = 1;
	  func_prefix = "ChronoLogThallium";
	}
	~ClocksourceManager()
	{}
	uint64_t TimeStamp()
	{
		return clocksource_->getTimeStamp()/unit+offset;
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

	void set_offset(uint64_t &o)
	{
		offset = o;
	}
	
private:
    std::shared_ptr<ChronoLogRPC> rpc;
    ClockSource *clocksource_;
    uint64_t unit;
    uint64_t offset;
    int num_procs;
    std::string func_prefix;
};

#endif //CHRONOLOG_CLOCKSOURCEMANAGER_H
