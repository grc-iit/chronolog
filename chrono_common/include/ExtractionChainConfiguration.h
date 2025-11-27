
#ifndef EXTRACTION_CHAIN_CONFIG_H
#define EXTRACTION_CHAIN_CONFIG_H

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
        
    
class ExtractionChainConfiguration
{
public:

    ExtractionChainConfiguration( std::string const&);
    ExtractionChainConfiguration( json_object * );

    int parse_json_conf(json_object*);

    bool is_valid() const;

    std::vector<ExtractorConfiguration> const & get_extractors() const {return extractors;}

private:
    std::vector<ExtractorConfiguration> extractors;
};
/*
std::string & to_string( std::string & a_string, ExtractionChainConfiguration const & extraction_chain_conf) 
{
    a_string += "{ ExtractionChainConfiguration ";
    
    for ( auto & extractor_conf : extraction_chain_conf.get_extractors())
        a_string += extractor_conf.to_string(a_string);        
    a_string += " }";

    return a_string;

}
*/
} // namespace chronolog
#endif
