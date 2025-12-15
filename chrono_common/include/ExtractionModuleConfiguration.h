
#ifndef EXTRACTION_MODULE_CONFIG_H
#define EXTRACTION_MODULE_CONFIG_H

#include <iostream>
#include <json-c/json.h>


namespace chronolog
{


class ExtractorConfiguration
{
public:

    ExtractorConfiguration(json_object* object=nullptr)
    : type_name("unknown")
    , json_object_conf(object)
    {}    

    virtual std::string const & get_type() const { return type_name; }
    virtual int parse_json_conf( json_object *) { return 1; };
    virtual bool is_valid() const { return true;};
    virtual std::string & to_string( std::string & a_string) const 
    { 
        a_string +="{ ExtractorConf { type: unknown} }" ;
        return a_string;
    }

    std::string type_name;
    json_object * json_object_conf;
};
        
    
class ExtractionModuleConfiguration
{
public:

    ExtractionModuleConfiguration( std::string const&);
    ExtractionModuleConfiguration( json_object * );

    
    int parse_json_conf(json_object*);
    int update_config(std::string const&);
    int update_config(json_object * );

    int get_extraction_thread_number() const { return extraction_thread_number; }
    std::vector<ExtractorConfiguration> const & get_extractors() const {return extractors;}

    bool is_valid() const;
    std::string & to_string(std::string &) const;

private:
    uint16_t    extraction_thread_number;
    std::vector<ExtractorConfiguration> extractors;
};

} // namespace chronolog
#endif
