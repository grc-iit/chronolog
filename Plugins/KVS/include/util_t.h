#ifndef __STRING_FUNCTIONS_H_
#define __STRING_FUNCTIONS_H_

#include "city.h"
#include <string>
#include <boost/container_hash/hash.hpp>
#include <mpi.h>
#include <hdf5.h>
#include <vector>
#include "event.h"

struct stringhash
{

   uint64_t operator()(std::string &s)
   {
        uint64_t hashvalue = std::hash<std::string>()(s);//CityHash64(s.c_str(),64);
        return hashvalue;
   }

};

struct stringequal
{

   bool operator()(std::string &s1,std::string &s2)
   {
        return (s1.compare(s2)==0);
   }
};

struct inthashfunc
{
   uint64_t operator()(int &k)
   {
      uint64_t key = std::hash<int>()(k);
      std::size_t seed = 0;
      boost::hash_combine(seed,key);
      key = seed;
      return key;
   }

};

struct unsignedlonghashfunc
{
   uint64_t operator()(uint64_t &k)
   {
     uint64_t key = std::hash<uint64_t>()(k);
     std::size_t seed = 0;
     boost::hash_combine(seed,key);
     key = seed;
     return key;
   }
};

struct floathashfunc
{
    uint64_t operator()(float &k)
    {
	uint64_t key = std::hash<float>()(k);
	std::size_t seed = 0;
	boost::hash_combine(seed,key);
	key = seed;
	return key;
    }
};

struct doublehashfunc
{
    uint64_t operator()(double &k)
    {
	uint64_t key = std::hash<double>()(k);
	std::size_t seed = 0;
	boost::hash_combine(seed,key);
	key = seed;	
	return key;
    }
};


int nearest_power_two(int n);
void create_integertestinput(std::string&,int,int,int,std::vector<int> &keys,std::vector<uint64_t> &ts);
void create_ycsb_input(std::string &,int,int,std::vector<uint64_t>&,std::vector<std::string>&,std::vector<int>&);
void create_timeseries_testinput(std::string&,int,int,std::vector<uint64_t>&,std::vector<float>&,std::vector<int>&);
void create_dataworld_testinput(std::string&,int,int,std::vector<uint64_t>&,std::vector<uint64_t>&,std::vector<int>&);
#endif
