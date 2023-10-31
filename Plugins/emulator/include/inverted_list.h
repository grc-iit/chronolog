#ifndef __INVERTED_LIST_H_
#define __INVERTED_LIST_H_

#include "rw.h"

template<class KeyT,class ValueT, class HashFcn=std::hash<KeyT>,class EqualFcn=std::equal_to<KeyT>>
struct invnode
{
   BlockMap<KeyT,ValueT,HashFcn,EqualFcn> *bm;
   memory_pool<KeyT,ValueT,HashFcn,EqualFcn> *ml; 
};

template<class KeyT,class ValueT,class hashfcn=std::hash<KeyT>,class equalfcn=std::equal_to<KeyT>>
struct head_node
{
  int keytype;
  int maxsize;
  struct invnode<KeyT,ValueT,hashfcn,equalfcn> *table;
};

template <class KeyT>
struct KeyIndex
{
  KeyT key;
  uint64_t index;
};

template<class KeyT>
bool compareIndex(struct KeyIndex<KeyT> &k1, struct KeyIndex<KeyT> &k2)
{
    if(k1.index < k2.index) return true;
    else return false;
}

template<class KeyT,class ValueT,class hashfcn=std::hash<KeyT>,class equalfcn=std::equal_to<KeyT>>
class hdf5_invlist
{

   private:
	   int numprocs;
	   int myrank;
	   std::unordered_map<std::string,struct head_node<KeyT,ValueT,hashfcn,equalfcn>*> invlists;
	   int tag;
	   hid_t kv1;
   public:
	   hdf5_invlist(int n,int p) : numprocs(n), myrank(p)
	   {
	     tag = 20000;
	     //kv1 = H5Tcreate(H5T_COMPOUND,sizeof(struct KeyIndex<int>));
    	     //H5Tinsert(kv1,"key",HOFFSET(struct KeyIndex<int>,key),H5T_NATIVE_INT);
    	     //H5Tinsert(kv1,"index",HOFFSET(struct KeyIndex<int>,index),H5T_NATIVE_INT);
	   }
	   ~hdf5_invlist()
	   {

		for(auto t = invlists.begin(); t != invlists.end(); ++t)
		{
		   auto r = t->first;
		   auto rv = t->second;
		   
		   if(rv->table != nullptr) 
		   {
			 delete rv->table->bm;
			 delete rv->table->ml;
			 delete rv->table;
		   }
		   delete rv;
		}
		//H5Tclose(kv1);

	   }

	   inline int nearest_power_two(int n)
	   {
		int c = 1;

		while(c < n)
		{
		   c = 2*c;
		}
		return c;
	   }

	   void create_invlist(std::string &,int);
	   void fill_invlist_from_file(std::string&,int);
	   void flush_table_file(std::string &,int);
	   int partition_no(KeyT &k);		  
	   void add_entries_to_tables(std::string&,std::vector<struct event>*,uint64_t,int); 
	   void get_entries_from_tables(std::string &,std::vector<std::vector<KeyT>> *,std::vector<std::vector<ValueT>>*,int&,int&);
};

#include "../srcs/inverted_list.cpp"

#endif
