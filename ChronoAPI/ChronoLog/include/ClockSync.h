#include <chrono>
#include <time.h>
#include <unistd.h>
#include <climits>

class ClocksourceCStyle
{
public:
    ClocksourceCStyle()
    {}
    ~ClocksourceCStyle()
    {}    
    uint64_t getTimestamp()
    {
        struct timespec t{};
        clock_gettime(CLOCK_TAI, &t);
        return t.tv_sec;
    }
};
 
class ClocksourceCPPStyle
{
public:
    ClocksourceCPPStyle()
    {}
    ~ClocksourceCPPStyle()
    {}    
    uint64_t getTimestamp()
    {
        auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count()/1000000000;
	return t;
    }
};


template<class Clocksource>
class ClockSynchronization
{
 
   private:
     Clocksource *clock;
     uint64_t maxError;

   public:
     ClockSynchronization()
     {}
     ~ClockSynchronization()
     {}
     
     uint64_t Timestamp()
     {
	 return clock->getTimestamp();
     }
     void SynchronizeClocks();
     void ComputeErrorInterval();

};
