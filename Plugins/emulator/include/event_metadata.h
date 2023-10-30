#ifndef __EVENT_METADATA_H_
#define __EVENT_METADATA_H_

#include <vector>
#include <algorithm>
#include <string>
#include <iostream>
#include <cmath>

class event_metadata
{

  private:
  int numattrs;
  std::string key_field;
  int psize;
  std::vector<std::string> attr_names;
  std::vector<int> value_sizes;
  std::vector<bool> is_signed;
  std::vector<bool> big_endian;
  int total_v;
  int chunksize;
  bool inverted_list;
  public:
      event_metadata()
      {
	  key_field = "timestamp";
	  psize = sizeof(uint64_t);
	  inverted_list = false;
      }
      ~event_metadata()
      {
      }
      bool get_invlist()
      {
	return inverted_list;
      }
      void set_invlist(bool b)
      {
	inverted_list = b;
      }
      event_metadata& operator=(event_metadata &m1)
      {
         numattrs = m1.get_numattrs();	
	 attr_names = m1.get_attr_names();
	 value_sizes = m1.get_value_sizes();
	 inverted_list = m1.get_invlist();
	 return *this; 
      }
      void set_numattrs(int n)
      {
	   numattrs = n;
      }
      int get_numattrs()
      {
	    return numattrs;
      }
      std::string &get_key_field()
      {
	      return key_field;
      }
      int ksize()
      {
	      return psize;
      }
      std::vector<std::string> &get_attr_names()
      {
	      return attr_names;
      }
      std::vector<int> &get_value_sizes()
      {
	      return value_sizes;
      }
      int get_datasize()
      {
	int n = 0;
	for(int i=0;i<value_sizes.size();i++)
		n += value_sizes[i];
	total_v = n;
	return total_v;
      }
      void add_attr(std::string &s,int size2,bool &sig, bool &end)
      {
	   attr_names.push_back(s);
	   value_sizes.push_back(size2);
	   is_signed.push_back(sig);
	   big_endian.push_back(end);
      }
      int get_chunksize()
      {
	int dsize = sizeof(uint64_t);
	for(int i=0;i<value_sizes.size();i++)
		dsize += value_sizes[i];
	double nevents = (double)CHUNKSIZE/(double)dsize;
	int nevents_i = ceil(nevents);
	return nevents_i; 
      }
      bool get_attr(std::string &s,int &v_size,bool &sign,bool &end)
      {
	   auto r = std::find(attr_names.begin(),attr_names.end(),s);
	   if(r != attr_names.end())
	   {
		int pos = std::distance(attr_names.begin(),r);
		v_size = value_sizes[pos];
		sign = is_signed[pos];
		end = big_endian[pos];
		return true;
	   }
	   return false;
      }
      bool get_offset(std::string &s,int &offset)
      {
	auto r = std::find(attr_names.begin(),attr_names.end(),s);
	if(r != attr_names.end())
	{
	   int pos = std::distance(attr_names.begin(),r);
	   int c = 0;
	   for(int i=0;i<pos;i++)
	   {
	        c += value_sizes[i];	
	   }
	   offset = c;
	   return true;
	}
	return false;
      }

      bool get_size(std::string &s,int &v)
      {
	auto r = std::find(attr_names.begin(),attr_names.end(),s);
	if(r != attr_names.end())
	{
	   int pos = std::distance(attr_names.begin(),r);
	   v = value_sizes[pos];
	   return true;
	}
	return false;
      }
};


#endif
