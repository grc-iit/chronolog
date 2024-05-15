
#include "DataStoreConfiguration.h"

chronolog::DataStoreConfiguration::DataStoreConfiguration()
{

}

std::string const& chronolog::DataStoreConfiguration::getJsonKey() const
{
    return std::string("DataStoreInternals");
}

chronolog::DataStoreConfiguration::DataStoreConfiguration(json_object & json_object_config)
{

}

int chronolog::DataStoreConfiguration::load(json_object & json_object_config)
{
    return 1;
}

std::string & chronolog::DataStoreConfiguration::to_string(std::string & a_string)
{
    return a_string;
}
    
std::ostream & chronolog::DataStoreConfiguration::to_stream(std::ostream & a_stream)
{

    return a_stream;
}

