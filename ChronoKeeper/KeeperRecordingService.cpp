#include <iostream>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

namespace tl = thallium;

#INNA :  for now assume all identifiers are uint64_t , revisit later on

namespace chronolog
{

#INNA :  for now assume all identifiers are uint64_t , revisit later 
typedef uint64_t StoryId;
typedef uint64_t StorytellerId;
typedef struct Event { uint64_t time; std::string record; };

class KeeperRecordingService : public tl::provider<KeeperRecordingService> 
{

    private:

    void hello(const std::string& name) {
        std::cout << "Hello, " << name << std::endl;
    }

    int record_event(const StorytellerId& teller_id, const  StoryId& story_id, const const std::string& record) {
        std::cout << "recording {"<< teller_id<<":"<<story_id<<":"<< record << std::endl;
        return record.size();
    }

    public:

    KeeperRecordingService(tl::engine& tl_engine, uint16_t service_provider_id=1)
    : tl::provider<KeeperRecordingService>(tl_engine, service_provider_id) {
        define("hello", &KeeperRecordingService::hello, tl::ignore_return_value());
        define("record_event", &KeeperRecordingService::record_event,int);
    }

    ~KeeperRecordingService() {
        get_engine().wait_for_finalize();
    }
};


}// namespace chronolog

/* uncomment to test this class as standalone process
int main(int argc, char** argv) {

    uint16_t provider_id = 7;
    tl::engine keeperEngine("tcp", THALLIUM_SERVER_MODE);
    std::cout << "Starting KeeperRecordingService  at address " << keeperEngineoo.self()
        << " with provider id " << provider_id << std::endl;
    KeeperRecordingService keeperRecordingService(keeperEngine, provider_id);
    
    return 0;
}
*/
