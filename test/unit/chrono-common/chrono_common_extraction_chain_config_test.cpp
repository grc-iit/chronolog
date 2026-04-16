#include <gtest/gtest.h>
#include <json-c/json.h>
#include <string>
#include <iostream>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <ExtractionModuleConfiguration.h>
#include <ChunkExtractorCSV.h>
#include <ChunkExtractorRDMA.h>


namespace chl = chronolog;

const char* logging_extractor_json_str = "{ \"extractror\" : {} }";

/*
JSON block for ExtractionModule Configuration is expected to look like this 

    "ExtractionModule": {
        "extraction_stream_count":2,
        "extraction_protocol": "ofi+sockets",
        "extractors": {           
            "test_csv_extractor": {
                "type": "csv_extractor",
                "csv_archive_dir": "/tmp/csv_archive"
            },
            "test_rdma_extractor": {
                "type": "single_endpoint_rdma_extractor",
                "grapher_receiving_endpoint": {
                    "protocol_conf": "ofi+sockets",
                    "service_ip": "127.0.0.1",
                    "service_base_port": 3333,
                    "service_provider_id": 33
                }
            },
            "test_dual_rdma_extractor": {
                "type": "dual_endpoint_rdma_extractor",
                "player_receiving_endpoint": {
                    "protocol_conf": "ofi+sockets",
                    "service_ip": "127.0.0.1",
                    "service_base_port": 2230,
                    "service_provider_id": 30
                },
                "grapher_receiving_endpoint": {
                    "protocol_conf": "ofi+sockets",
                    "service_ip": "127.0.0.1",
                    "service_base_port": 3333,
                    "service_provider_id": 33
                }
            }
        }
    }
*/


std::string rdma_extractor_json_string = std::string("{") + "\"test_rdma_extractor\": { " +
                                         "\"type\": \"single_endpoint_rdma_extractor\", " +
                                         "\"receiving_endpoint\": { \"protocol_conf\": \"ofi+sockets\",\"service_ip\": "
                                         "\"127.0.0.1\", \"service_base_port\": 3333,\"service_provider_id\": 33 } } " +
                                         "}";

std::string extraction_module_json_string =
        std::string("{ \"ExtractionModule\": { \"extraction_stream_count\":2,") + "\"extractors\": { " +
        "\"test_csv_extractor\": { \"type\": \"csv_extractor\", \"csv_archive_dir\": \"/tmp/csv_archive\" }"

        + "," + "\"test_rdma_extractor\": { " + "\"type\": \"single_endpoint_rdma_extractor\", " +
        "\"receiving_endpoint\": { \"protocol_conf\": \"ofi+sockets\",\"service_ip\": \"127.0.0.1\", "
        "\"service_base_port\": 3333,\"service_provider_id\": 33 } } " +
        "} }" + "}";


/////////
int test_csv_extractor_config()
{
    std::string csv_extractor_json_string =
            std::string("{") +
            " \"test_csv_extractor\": { \"type\": \"csv_extractor\", \"csv_archive_dir\": \"/tmp/csv_archive\" }" + "}";

    json_object* parsed_json = json_tokener_parse(csv_extractor_json_string.c_str());

    json_object* extractor_block = json_object_object_get(parsed_json, "test_csv_extractor");

    if(!json_object_is_type(extractor_block, json_type_object))
    {
        std::cout << "\n test_csv_extractor_config " << " failed : json_block  is not a json object" << std::endl;
        return -1;
    }

    chl::StoryChunkExtractorCSV csv_extractor(chl::ServiceId{});

    int return_value = csv_extractor.reset(extractor_block);

    json_object_put(parsed_json);

    return return_value;
}

int test_rdma_extractor_config(tl::engine& tl_engine, std::string const& json_string)
{
    json_object* parsed_json = json_tokener_parse(json_string.c_str());

    json_object* extractor_block = json_object_object_get(parsed_json, "test_rdma_extractor");

    if(!json_object_is_type(extractor_block, json_type_object))
    {
        std::cout << "\n test_rdma_extractor_config " << " failed : json_block  is not a json object" << std::endl;
        return -1;
    }

    chl::StoryChunkExtractorRDMA single_endpoint_rdma_extractor(tl_engine, chl::ServiceId());

    int return_value = single_endpoint_rdma_extractor.reset(extractor_block);

    json_object_put(parsed_json);

    return return_value;
}

int main()
{

    int result = chronolog::chrono_monitor::initialize(
            "file" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
            ,
            "/tmp/extraction_test.log" //, confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
            ,
            chronolog::LogLevel::trace // confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
            ,
            "ExtractionModuleTest" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
            ,
            100000000 //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
            ,
            2 //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
            ,
            chronolog::LogLevel::trace //confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
    );

    // Test 1 :  csv_extractor configuration

    int test_ret_value = test_csv_extractor_config();
    if(test_ret_value == chl::CL_SUCCESS)
    {
        std::cout << "\n [ExtractionModuleConfiguration] Test 1 csv_extractor_config:" << " passed " << std::endl;
    }
    else
    {
        std::cout << "\n [ExtractionModuleConfiguration] Test 1 csv_extractor_config:" << " failed " << std::endl;
        return (-1);
    }

    // Test 2 : single endpoint rdma extractor
    // reset receiver configuration
    chl::ServiceId localServiceId("ofi+sockets", "127.0.0.1", 2225, 25);

    std::string LOCAL_SERVICE_NA_STRING;
    localServiceId.get_service_as_string(LOCAL_SERVICE_NA_STRING);

    tl::engine* localEngine = nullptr;

    try
    {
        margo_instance_id margo_id = margo_init(LOCAL_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        localEngine = new tl::engine(margo_id);
    }
    catch(tl::exception const&)
    {
        return (-1);
    }


    test_ret_value = test_rdma_extractor_config(*localEngine, rdma_extractor_json_string);
    if(test_ret_value == chl::CL_SUCCESS)
    {
        std::cout << "\n [ExtractionModuleConfiguration] Test 2 rdma_extractor_config:" << " passed " << std::endl;
    }
    else
    {
        std::cout << "\n [ExtractionModuleConfiguration] Test 2 rdma_extractor_config:" << " failed " << std::endl;
        return (-1);
    }

    // Test 3 ExtractionModule Configuration

    struct json_object *json_block, *parsed_json, *tags_array, *temp_item;

    // Parse the full JSON string
    json_block = json_tokener_parse(extraction_module_json_string.c_str());

    chronolog::ExtractionModuleConfiguration module_config;


    if(!json_object_is_type(json_object_object_get(json_block, "ExtractionModule"), json_type_object))
    {
        std::cerr << "\n [ExtractionModuleConfiguration] Test parsing stage:"
                  << " failed : json_block  is not a json object" << std::endl;
        return -1;
    }

    json_object* extraction_module_block = json_object_object_get(json_block, "ExtractionModule");
    int return_value = module_config.parse_json_object(extraction_module_block);

    std::string log_string;
    module_config.to_string(log_string);

    std::cout << "\n [ExtractionModuleConfiguration] Test parsing stage:" << " passed " << log_string << std::endl;

    json_object_put(json_block);

    // confirm that borrowed extractor_json_objects in ExtractionModuleConfiguration instance are retained
    // after the original json_block object is released

    for(auto iter = module_config.extractors.begin(); iter != module_config.extractors.end(); ++iter)
    {
        if(!json_object_is_type((*iter).second, json_type_object))
        {
            std::cerr << "\n [ExtractionModuleConfiguration] Test borrowed reference stage:"
                      << " failed to retain extractor_json_object for" << (*iter).first << std::endl;
            return (-1);
        }
    }

    log_string.clear();

    std::cout << "\n [ExtractionModuleConfiguration] Test borowed reference stage:" << " passed :"
              << module_config.to_string(log_string) << std::endl;


    return return_value;
}
