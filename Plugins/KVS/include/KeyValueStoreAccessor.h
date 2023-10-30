#ifndef __KeyValueStoreAccessor_H_
#define __KeyValueStoreAccessor_H_

#include "KeyValueStoreMetadata.h"
#include "invertedlist.h"
#include "external_sort.h"
#include "util_t.h"

#include <thallium.hpp>
#include <thallium/serialization/proc_input_archive.hpp>
#include <thallium/serialization/proc_output_archive.hpp>
#include <thallium/serialization/serialize.hpp>
#include <thallium/serialization/stl/array.hpp>
#include <thallium/serialization/stl/complex.hpp>
#include <thallium/serialization/stl/deque.hpp>
#include <thallium/serialization/stl/forward_list.hpp>
#include <thallium/serialization/stl/list.hpp>
#include <thallium/serialization/stl/map.hpp>
#include <thallium/serialization/stl/multimap.hpp>
#include <thallium/serialization/stl/multiset.hpp>
#include <thallium/serialization/stl/pair.hpp>
#include <thallium/serialization/stl/set.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/tuple.hpp>
#include <thallium/serialization/stl/unordered_map.hpp>
#include <thallium/serialization/stl/unordered_multimap.hpp>
#include <thallium/serialization/stl/unordered_multiset.hpp>
#include <thallium/serialization/stl/unordered_set.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cassert>
#include "KeyValueStoreIO.h"
#include "Interface_Queues.h"
#include <mutex>

namespace tl=thallium;

typedef hdf5_invlist<int,uint64_t,inthashfunc,std::equal_to<int>> integer_invlist;
typedef hdf5_invlist<uint64_t,uint64_t,unsignedlonghashfunc,std::equal_to<uint64_t>> unsigned_long_invlist;
typedef hdf5_invlist<float,uint64_t,floathashfunc,std::equal_to<float>> float_invlist;
typedef hdf5_invlist<double,uint64_t,doublehashfunc,std::equal_to<double>> double_invlist;

class KeyValueStoreAccessor
{

   private :
	    int numprocs;
	    int myrank;
	    KeyValueStoreMetadata md;
	    std::vector<std::pair<std::string,void*>> lists;
	    std::unordered_map<std::string,int> secondary_attributes;
	    KeyValueStoreIO *kio;
	    Interface_Queues *if_q;
	    data_server_client *d;
	    std::mutex accessor_mutex;
	    int inserts;

   public :
	  KeyValueStoreAccessor(int np,int p,KeyValueStoreMetadata &m,KeyValueStoreIO *io,Interface_Queues *ifq,data_server_client *ds)
          {
		numprocs = np;
		myrank = p;
		md = m;
		kio = io;
		if_q = ifq;
		d = ds;
		inserts=0;
	  }

	  KeyValueStoreMetadata & get_metadata()
	  {
		return md;
	  }
	  int get_inserts()
	  {
		return inserts;
	  }

	  std::string get_attribute_type(std::string &attr_name)
	  {
		return md.get_type(attr_name);
	  }

	  int create_invertedlist(std::string &attr_name,int c,int maxsize)
	  {
		std::string name = md.db_name();
		std::string type = md.get_type(attr_name);

		int numtables = numprocs;
		if(type.empty()) return -1;
	
		auto r = secondary_attributes.find(attr_name);
		int ret; 
		if(r == secondary_attributes.end())
		{
		  
		  if(type.compare("int")==0)
		  {
		   int maxint = INT32_MAX;
		   ret = add_new_inverted_list<integer_invlist,int>(name,attr_name,maxsize,numtables,maxint,d,kio,if_q,c,md.value_size());
		  }
		  else if(type.compare("unsignedlong")==0)
		  {
		   uint64_t maxuint = UINT64_MAX;
		   ret = add_new_inverted_list<unsigned_long_invlist,uint64_t>(name,attr_name,maxsize,numtables,maxuint,d,kio,if_q,c,md.value_size());
		  }
		  else if(type.compare("float")==0)
		  {
		   float maxfl = DBL_MAX;
		   ret = add_new_inverted_list<float_invlist,float>(name,attr_name,maxsize,numtables,maxfl,d,kio,if_q,c,md.value_size());
		  }
		  else if(type.compare("double")==0)
		  {
		   double maxd = DBL_MAX;
		   ret = add_new_inverted_list<double_invlist,double>(name,attr_name,maxsize,numtables,maxd,d,kio,if_q,c,md.value_size());
		  }
		}
		else ret = r->second;
		return ret;
	  }

	  int get_inverted_list_index(std::string &attr_name)
	  {
		int ret = -1;
		auto r = secondary_attributes.find(attr_name);
		if(r==secondary_attributes.end()) return ret;

		return r->second;
	  }

	  template<typename T,typename N>
	  int add_new_inverted_list(std::string &,std::string &,int,int,N&,data_server_client*,KeyValueStoreIO*,Interface_Queues*,int,int);
	  template<typename T>
	  bool delete_inverted_list(int);
	  template<typename T,typename N>
	  bool insert_entry(int, N&key,uint64_t &ts);
	  template<typename T>
	  bool find_entry(int,T &key);
	  template<typename T,typename N>
	  std::vector<uint64_t> get_entry(int,N &key);
	  template<typename T>
	  void flush_invertedlist(std::string &,bool);
	  template<typename T>
	  void fill_invertedlist(int);
	  template <typename T,typename N,typename M>
	  bool Put(int,std::string &,N &key, M &value);
	  template<typename T,typename N,typename M>
	  uint64_t Put_ts(int,std::string&,N &key,M &value);
	  template <typename T,typename N>
          bool Get(int,std::string&,N&,int);
	  template<typename T,typename N>
	  bool Get_resp(int,std::string&,N&,int);
	  void sort_on_secondary_key(std::string &attr_name);
	  template<typename T,typename N>
	  std::vector<std::pair<int,std::string>> Completed_Gets(int,std::string&);
	  ~KeyValueStoreAccessor()
	  {
		for(int i=0;i<lists.size();i++)
		{
		   std::string attr_name = lists[i].first;
		   std::string type = md.get_type(attr_name);
		   bool ret = false;
		   if(type.compare("int")==0) ret = delete_inverted_list<integer_invlist>(i);
		   else if(type.compare("unsignedlong")==0) ret = delete_inverted_list<unsigned_long_invlist>(i);
		   else if(type.compare("float")==0) ret = delete_inverted_list<float_invlist>(i);
		   else if(type.compare("double")==0) ret = delete_inverted_list<double_invlist>(i);
		}
	  }

};

#include "../srcs/KeyValueStoreAccessor.cpp"

#endif
