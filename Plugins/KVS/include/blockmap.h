#ifndef HCL_BLOCK_MAP_H
#define HCL_BLOCK_MAP_H

#include <vector>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <cassert>
#include <atomic>
#include <memory>
#include <type_traits>
#include <string>
#include "memoryallocation.h"
#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/cstdint.hpp>
#include <boost/config.hpp>
#include <boost/atomic.hpp>
//#include <boost/thread/lock_types.hpp>

#define NOT_IN_TABLE UINT64_MAX
#define EXISTS 1
#define INSERTED 0

template <
	class KeyT,
	class ValueT,
	class HashFcn=std::hash<KeyT>,
	class EqualFcn=std::equal_to<KeyT>>
struct f_node
{
    uint64_t num_nodes;
    boost::shared_mutex mutex_t;
    struct node<KeyT,ValueT,HashFcn,EqualFcn> *head;
};

template <
    class KeyT,
    class ValueT, 
    class HashFcn = std::hash<KeyT>,
    class EqualFcn = std::equal_to<KeyT>>
class BlockMap
{

   public :
	typedef struct node<KeyT,ValueT,HashFcn,EqualFcn> node_type;
	typedef struct f_node<KeyT,ValueT,HashFcn,EqualFcn> fnode_type;
   private :
	std::vector<fnode_type> *table;
	uint64_t maxSize;
	std::atomic<uint64_t> allocated;
	std::atomic<uint64_t> removed;
	memory_pool<KeyT,ValueT,HashFcn,EqualFcn> *pl;
	KeyT emptyKey;

	uint64_t KeyToIndex(KeyT &k)
	{
	    uint64_t hashval = HashFcn()(k);
	    uint64_t pos = hashval%maxSize;
	    return pos;
	}
  public:

	BlockMap(uint64_t n,memory_pool<KeyT,ValueT,HashFcn,EqualFcn> *m,KeyT maxKey) : maxSize(n), pl(m), emptyKey(maxKey)
	{
  	   assert (maxSize > 0);
	   table = new std::vector<fnode_type> (maxSize); 
	   assert (table != nullptr);
	   for(size_t i=0;i<maxSize;i++)
	   {
	      (*table)[i].num_nodes = 0;
	      (*table)[i].head = pl->memory_pool_pop();
	      new (&((*table)[i].head->key)) KeyT(emptyKey);
	      (*table)[i].head->next = nullptr; 
	   }
	   allocated.store(0);
	   removed.store(0);
	   assert(maxSize < UINT64_MAX);
	}

  	~BlockMap()
	{
	   delete table;
	}

	uint32_t insert(KeyT &k,ValueT &v)
	{
	    uint64_t pos = KeyToIndex(k);

	    boost::unique_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);

	    node_type *p = (*table)[pos].head;
	    node_type *n = (*table)[pos].head->next;


	    bool found = false;
	    while(n != nullptr)
	    {
		//if(EqualFcn()(n->key,k)) found = true;
		if(HashFcn()(n->key)>HashFcn()(k)) 
		{
		   break;
		}
		p = n;
		n = n->next;
	    }

	    uint32_t ret = (found) ? EXISTS : 0;
	    if(!found)
	    {
		allocated.fetch_add(1);
		node_type *new_node=pl->memory_pool_pop();
		new (&(new_node->key)) KeyT(k);
		new (&(new_node->value)) ValueT(v);
		new_node->next = n;
		p->next = new_node;
		(*table)[pos].num_nodes++;
		found = true;
		ret = INSERTED;
	    }

	   return ret;
	}

	uint64_t find(KeyT &k)
	{
	    uint64_t pos = KeyToIndex(k);

	    boost::shared_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);

	    node_type *n = (*table)[pos].head->next;
	    bool found = false;
	    while(n != nullptr)
	    {
		if(EqualFcn()(n->key,k))
		{
		   found = true;
		}
		if(HashFcn()(n->key) > HashFcn()(k)) break;
		n = n->next;
	    }

	    return (found ? pos : NOT_IN_TABLE);
	}

	bool update(KeyT &k,ValueT &v)
	{
	   uint64_t pos = KeyToIndex(k);

	   boost::unique_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);
	   node_type *n = (*table)[pos].head->next;

	   bool found = false;
	   while(n != nullptr)
	   {
		if(EqualFcn()(n->key,k))
		{
		   found = true;
		   n->value = v;
		}
		if(HashFcn()(n->key) > HashFcn()(k)) break;
		n = n->next;
	   }

	   return found;
	}

	bool get(KeyT &k,ValueT *v)
	{
	    bool found = false;

	    uint64_t pos = KeyToIndex(k);

	    boost::shared_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);

	    node_type *n = (*table)[pos].head->next;

	    while(n != nullptr)
	    {
		if(EqualFcn()(n->key,k))
		{
		   found = true;
		   *v = n->value;
		   break;
		}
		if(HashFcn()(n->key) > HashFcn()(k)) break;
		n = n->next;
	    }

	   return found;
	}

	bool getvalues(KeyT &k,std::vector<ValueT> &values)
	{
	   bool found = false;

	   uint64_t pos = KeyToIndex(k);

	   boost::shared_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);

	   node_type *n = (*table)[pos].head->next;

	   while(n != nullptr)
	   {
	      if(EqualFcn()(n->key,k))
	      {
		values.push_back(n->value);
	      }
	      if(HashFcn()(n->key) > HashFcn()(k)) break;
	      n = n->next;

	   }

	   if(values.size() > 0) found = true;
	   return found;

	}

	template<typename... Args>
	bool update_field(KeyT &k,void(*fn)(ValueT *,Args&&... args),Args&&... args_)
	{
	    bool found = false;
	    uint64_t pos = KeyToIndex(k);

	    boost::unique_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);

	    node_type *n = (*table)[pos].head->next;

	    while(n != nullptr)
	    {
		if(EqualFcn()(n->key,k))
		{
		    found = true;
		    fn(&(n->value),std::forward<Args>(args_)...);
		}
		if(HashFcn()(n->key) > HashFcn()(k)) break;
		n = n->next; 
	    }

	    return found;
	}

	template<typename... Args>
	bool erase_if(KeyT &k,bool(*fn)(ValueT *,Args&&... args),Args&&... args_)
	{
	  bool found = false;
	  uint64_t pos = KeyToIndex(k);
	  boost::unique_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);

	  node_type *p = (*table)[pos].head;
	  node_type *n = (*table)[pos].head->next;
          bool b = false;

	  while(n != nullptr)
	  {
	     if(EqualFcn()(n->key,k))
	     {
		  b = fn(&(n->value),std::forward<Args>(args_)...);
		  break;
	     }
	     if(HashFcn()(n->key) > HashFcn()(k)) break;
	     p = n;
	     n = n->next;
	  }
	  
	  if(n != nullptr)
	  if(EqualFcn()(n->key,k) && b)
	  {
	     found = true;
	     p->next = n->next;
	     pl->memory_pool_push(n);
	     (*table)[pos].num_nodes--;
	     removed.fetch_add(1);
	  }
	  return found;
	}
	bool erase(KeyT &k)
	{
	   uint64_t pos = KeyToIndex(k);

	   boost::unique_lock<boost::shared_mutex> lk((*table)[pos].mutex_t);

	   node_type *p = (*table)[pos].head;
	   node_type *n = (*table)[pos].head->next;

	   bool found = false;

	   while(n != nullptr)
	   {
		if(EqualFcn()(n->key,k)) break;

		if(HashFcn()(n->key) > HashFcn()(k)) break;
		p = n;
		n = n->next;
	  }
         
	  if(n != nullptr)
	  if(EqualFcn()(n->key,k))
	  {
		found = true;
		p->next = n->next;
		pl->memory_pool_push(n);
		(*table)[pos].num_nodes--;
		removed.fetch_add(1);
	  }
	 
	   return found;
	}

	uint64_t allocated_nodes()
	{
		return allocated.load();
	}

	uint64_t removed_nodes()
	{
		return removed.load();
	}

	uint64_t count_block_entries()
	{
	   uint64_t num_entries = 0;
	   for(size_t i=0;i<maxSize;i++)
	   {
		num_entries += (*table)[i].num_nodes;
	   }
	   return num_entries;
	}

	bool clear_map()
	{
	  for(int i=0;i<maxSize;i++)
	  {
             boost::unique_lock<boost::shared_mutex> lk((*table)[i].mutex_t);

	     node_type *n = (*table)[i].head->next;
	     while(n != nullptr)
	     {
		node_type *nn = n->next;
		n->next = nullptr;
		pl->memory_pool_push(n);
		n = nn;
		(*table)[i].num_nodes--;
	     }
	     (*table)[i].head->next = nullptr; 
	  }
	  return true;
	}

	bool fetch_clear_map(std::vector<std::vector<KeyT>> *keys, std::vector<std::vector<ValueT>> *values, ValueT &maxv)
	{
	    keys->resize(maxSize);
    	    values->resize(maxSize);

	    for(int i=0;i<maxSize;i++)
	    {
		boost::unique_lock<boost::shared_mutex> lk((*table)[i].mutex_t);
		node_type *n = (*table)[i].head->next;
		node_type *p = (*table)[i].head;
	
		while(n != nullptr)
		{
		    if(n->value <= maxv)
		    {
			(*keys)[i].push_back(n->key);
			(*values)[i].push_back(n->value);
			node_type *t = n;
			p->next = n->next;
			n = n->next;
			pl->memory_pool_push(t);
		    }
		    else
		    {
			p = n;
			n = n->next;
		    }
		}


	    }
	    return true;
	}

	bool get_map(std::vector<ValueT> &values)
	{
	   int num_entries = 0;
	   for(int i=0;i<maxSize;i++)
	   {
	     boost::shared_lock<boost::shared_mutex> lk((*table)[i].mutex_t);

	     node_type *n = (*table)[i].head->next;

	     while(n != nullptr)
	     {
		values.push_back(n->value);
		n = n->next;
		num_entries++;
	     }
	   }
	   //std::cout <<" num_entries = "<<num_entries<<" allocated = "<<allocated.load()<<std::endl;
	   return true;
	}

	bool get_map_keyvalue(std::vector<std::vector<KeyT>> *keys,std::vector<std::vector<ValueT>> *values)
	{
	   keys->resize(maxSize);
	   values->resize(maxSize);
	   int num_entries=0;
	   for(int i=0;i<maxSize;i++)
	   {
		boost::shared_lock<boost::shared_mutex> lk((*table)[i].mutex_t);

		node_type *n = (*table)[i].head->next;

		while(n != nullptr)
		{
		   (*keys)[i].push_back(n->key);
		   (*values)[i].push_back(n->value);
		   n = n->next;
		   num_entries++;
		}
	   }
	   return false;
	}	
};

#endif
