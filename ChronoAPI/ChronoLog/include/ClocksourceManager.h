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
	ClocksourceManager()
	{
	  unit = 1000;
	  offset = 0;
	  num_procs = 1;
	  func_prefix = "ChronoLogThallium";
	  maxError = UINT64_MAX;
	  errs.resize(num_procs);
	  std::fill(errs.begin(),errs.end(),UINT64_MAX);
	  pos_neg = false;
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

	     if(myid.compare(client_id)==0) return t;

	     std::lock_guard<std::mutex> lock_c(clock_m);

	     if(std::find(client_ids.begin(),client_ids.end(),client_id)==client_ids.end())
	     {
		     client_ids.insert(client_id);
		     num_reqs.push_back(0);
	     }

	     int pos = std::distance(client_ids.begin(),std::find(client_ids.begin(),client_ids.end(),client_id));
	     num_reqs[pos]++;

	     return t;

	}

	int LocalStoreError(std::string &client_id,uint64_t &t)
	{	
	   std::lock_guard<std::mutex> lock_c(clock_m);
	   std::cout <<" se = "<<t<<std::endl;
	   int pos = std::distance(client_ids.begin(),std::find(client_ids.begin(),client_ids.end(),client_id));
	   errs[pos] = t;
	   return CL_SUCCESS;
	}
        
	uint64_t LocalMaxError(std::string &client_id)
	{
	   bool b = true;

	   std::lock_guard<std::mutex> lock_c(clock_m);

           for(int i=0;i<num_procs;i++)
	   {
	      if(errs[i] == UINT64_MAX)
	      {
		   b = false; break;
	      }
	   }

	   if(!b) return UINT64_MAX;
	   
	   if(maxError != UINT64_MAX) return maxError;
	  
	   uint64_t t = 0;
	   for(int i=0;i<num_procs;i++)
		   if(t < errs[i]) t = errs[i];
	
	   maxError = t;
	   return maxError;
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

		std::function<void(const tl::request &,
				   std::string &,
				   uint64_t &)> errorIntervalFunc(
				   [this](auto && PH1,
					  auto && PH2,
					  auto && PH3) {
				   ThalliumLocalStoreError(std::forward<decltype(PH1)>(PH1),
						           std::forward<decltype(PH2)>(PH2),
							   std::forward<decltype(PH3)>(PH3));
				   });

		std::function<void(const tl::request &,
				   std::string &)> getErrorFunc(
				   [this](auto && PH1,
					  auto && PH2) {
				   ThalliumLocalMaxError(std::forward<decltype(PH1)>(PH1),
						         std::forward<decltype(PH2)>(PH2));
				   });
				
		rpc->bind("ChronoLogThalliumGetTS", timestampFunc);
		rpc->bind("ChronoLogThalliumStoreError", errorIntervalFunc);
		rpc->bind("ChronoLogThalliumGetMaxError",getErrorFunc);

	}


	CHRONOLOG_THALLIUM_DEFINE(LocalGetTS,(client_id),std::string& client_id)
	CHRONOLOG_THALLIUM_DEFINE(LocalStoreError,(client_id,time),std::string& client_id,uint64_t time)
	CHRONOLOG_THALLIUM_DEFINE(LocalMaxError,(client_id),std::string &client_id)

	void set_offset(uint64_t &o, bool &b)
	{
		offset = o;
		pos_neg = b;
	}
	void set_myid(std::string &s)
	{
		myid = s;
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
    int num_procs;
    std::string func_prefix;
    uint64_t maxError;
    uint64_t epsilon;
    std::vector<int> num_reqs;
    std::vector<uint64_t> errs;
    std::string myid;
    std::set<std::string> client_ids;
    std::mutex clock_m;
};

#endif //CHRONOLOG_CLOCKSOURCEMANAGER_H
