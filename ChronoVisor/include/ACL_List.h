#include <ACL.h>
#include "city.h"
#include <unordered_map>
#include <mutex>

class ACL_List
{

 private:
     std::unordered_map<std::string,std::vector<ACL_Map>,stringhashfn> *acls;
     std::mutex acl_mutex;

  public:
        ACL_List()
	{
	   acls = new std::unordered_map<std::string,std::vector<ACL_Map>,stringhashfn> ();

	}
        ~ACL_List()
	{
	    delete acls;
	}

	inline bool create_acl(std::string &name,std::string &group_id,std::string &perm)
	{
	     std::lock_guard<std::mutex> lock_g(acl_mutex);

             auto l = acls->find(name);
	     ACL a; a.set_permission(perm);
	     ACL_Map m; m.create_acl_map(group_id,a);
             bool r = false;
	     bool exists = false;

	     if(l != acls->end())
	     {
	          for(auto t = l->second.begin(); t != l->second.end(); ++t)
		  {	  
		     if(t->get_group_id().compare(group_id)==0)
		     {
			 exists = true; break;
		     }
		  }

		  if(!exists)
		  {
		     l->second.emplace(l->second.end(),m);
		     r = true;
		  }
	     }
             else
	     {
		std::vector<ACL_Map> nl;
		nl.push_back(m);
		std::pair<std::string,std::vector<ACL_Map>> p(name,nl);
		acls->insert(p);
		r = true;
	     } 
	     return r;
	}

	inline bool remove_acl(std::string &name,std::string &group_id)
	{
	   std::lock_guard<std::mutex> lock_g(acl_mutex);
	
   	   auto l = acls->find(name);	   
	   bool ret = false;

	   if(l != acls->end())
	   {
		for(auto t = l->second.begin(); t != l->second.end();)
		{
		  if((*t).get_group_id().compare(group_id)==0)
		  {
			  l->second.erase(t);
			  ret = true;
			  break;
		  }
		  else ++t;
		}
	   }
	   return ret;
	}

	inline void remove_all_acls(std::string &name)
	{
	    std::lock_guard<std::mutex> lock_g(acl_mutex);

	    acls->erase(name);
	}
        inline bool is_owner(std::string &name, std::string &group_id)
	{
	   std::lock_guard<std::mutex> lock_g(acl_mutex);

	   auto l = acls->find(name);

	   bool ret = false;
	   if(l != acls->end())
	   {
		   for(auto t = l->second.begin(); t != l->second.end(); ++t)
		   {
			   if(t->get_group_id().compare(group_id)==0)
			   {
				ret = true;
				break;
			   }
		   }
	   }
	   return ret;
	}
	inline bool is_allowed(std::string &name,std::string &group_id,enum ChronoLogOp& op)
	{
	   std::lock_guard<std::mutex> lock_g(acl_mutex);

	   auto l = acls->find(name);
	   int ret = false;

	   if(l != acls->end())
	   {
	        for(auto t = l->second.begin(); t != l->second.end(); ++t)
		{
		    if(t->get_group_id().compare(group_id)==0)
		    {
			ACL a = t->get_acl();	    
	   	        if(a.get_op_permission(op)) ret = true;	
			break;
		    }
		}
	   }
	   return ret;
	}

	inline bool allow_op(std::string &name, std::string &group_id,enum ChronoLogOp &op)
	{
	    std::lock_guard<std::mutex> lock_g(acl_mutex);

	    auto l = acls->find(name);
	    bool ret = false;

	    if(l != acls->end())
	    {
		  for(auto t=l->second.begin(); t != l->second.end(); ++t)
		  {
		       if(t->get_group_id().compare(group_id)==0)
		       {
			  t->allow_acl_op(op);
			  ret = true;
			  break;
		       }
		  }
	    }
	    return ret;
	}
        inline bool deny_op(std::string &name,std::string &group_id,enum ChronoLogOp &op)
	{
	   std::lock_guard<std::mutex> lock_g(acl_mutex);

	   auto l = acls->find(name);
	   bool ret = false;

	   if(l != acls->end())
	   {
		 for(auto t = l->second.begin(); t != l->second.end(); ++t)
		 {
		    if(t->get_group_id().compare(group_id)==0)
		    {
			 t->deny_acl_op(op);
			 ret = true;
			 break;
		    }
		 }
	   }
	   return ret;
	}

	inline bool replace_acl(std::string &name, std::string &group_id, uint32_t &p)
	{
	   std::lock_guard<std::mutex> lock_g(acl_mutex);

	   auto l = acls->find(name);
	   int ret = false;

	   if(l != acls->end())
	   {
		 for(auto t = l->second.begin(); t != l->second.end(); ++t)
		 {
		     if(t->get_group_id().compare(group_id)==0)
		     {
			   t->modify_acl(p);
			   ret = true;
			   break;
		     }
		 }
	   }
	    return ret;
	}

};
