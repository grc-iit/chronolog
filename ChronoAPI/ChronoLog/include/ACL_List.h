#ifndef ACL_LIST_H
#define ACL_LIST_H
#include <ACL.h>
#include "city.h"
#include <unordered_map>
#include <mutex>

class ACL_DB
{

 private:
     std::unordered_map<std::string,std::vector<ACL_Map>,stringhashfn> *acls;
     std::mutex acl_mutex;

  public:
        ACL_DB()
	{
	   acls = new std::unordered_map<std::string,std::vector<ACL_Map>,stringhashfn> ();

	}
        ~ACL_DB()
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
		std::cout <<" create acl list"<<std::endl;
		std::vector<ACL_Map> nl;
		nl.push_back(m);
		std::pair<std::string,std::vector<ACL_Map>> p(name,nl);
		acls->insert(p);
		r = true;
	     } 
	     return r;
	}

	inline bool create_acl(std::vector<std::string> &names, std::vector<std::string> &groups, std::vector<std::string> &perms)
	{
	   int num_acls = names.size();
	   std::lock_guard<std::mutex> lock_g(acl_mutex);

	   bool r = true;

	   for(int i=0;i<num_acls;i++)
	   {
		auto l = acls->find(names[i]);
		ACL a; a.set_permission(perms[i]);
	        ACL_Map m; m.create_acl_map(groups[i],a);
		bool exists = false;
		
		if(l != acls->end())
		{
		   for(auto t = l->second.begin(); t != l->second.end(); ++t)
		   {
			if(t->get_group_id().compare(groups[i])==0)
			{
				exists = true; break;
			}
		   }
		   if(!exists)
		   {
			l->second.emplace(l->second.end(),m);
		   }
		   else r=false;
		}
		else
		{
		   std::vector<ACL_Map> nl;
		   nl.push_back(m);
		   std::pair<std::string,std::vector<ACL_Map>> p(names[i],nl);
		   acls->insert(p);

		}

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
	    std::cout <<" removing entry name = "<<name<<std::endl;
	    acls->erase(name);
	}
	inline bool check_remove_all_acls(std::string &name, std::string &group_id)
	{
	  std::lock_guard<std::mutex> lock_g(acl_mutex);

	  auto l = acls->find(name);
	
	  bool found = false;
	  if(l != acls->end())
	  {
		for(auto t = l->second.begin();t != l->second.end(); ++t)
		{
		   if(t->get_group_id().compare(group_id)==0)
		   {	
			ACL a = t->get_acl();
			enum ChronoLogOp nop = CHRONOLOG_CREATE_DELETE;
			if(a.get_op_permission(nop))
			{
			   found = true;
			}
	     	        break;
		    }
   		}		   
	  }
	
	  if(found) acls->erase(name);
	  return found;
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

	inline bool replace_acl(std::string &name, std::string &group_id, std::string &p)
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


class ACL_List
{
  private:
	  std::vector<ACL_Map> acls;

  public: 
	  ACL_List() 
	  {}
	  ~ACL_List()
	  {}

           inline bool create_acl(std::string &group_id,std::string &perm)
	   {
		bool exist = false;
		bool ret = false;

		for(auto t = acls.begin(); t != acls.end(); ++t)
		{
			if(t->get_group_id().compare(group_id)==0)
			{
			   exist = true; break;
			}
		}

		if(!exist)
		{
		   ACL a; a.set_permission(perm);
		   ACL_Map m; m.create_acl_map(group_id,a);
		   acls.push_back(m);
		   std::cout <<" create acl"<<std::endl;
		   ret = true;
		}
		return ret;
	   }
	
	   inline bool remove_acl(std::string &group_id)
	   {
		bool ret = false;

		for(auto t=acls.begin(); t!=acls.end();)
		{
			if(t->get_group_id().compare(group_id)==0)
			{
				ret = true;
				acls.erase(t);
				break;
			}
			else ++t;
		}
		return ret;
	   }

	   inline bool is_allowed(std::string &group_id,enum ChronoLogOp &op)
	   {
		   bool ret = false;

		   for(auto t=acls.begin(); t != acls.end(); ++t)
		   {
			   if(t->get_group_id().compare(group_id)==0)
			   {
				   if(t->is_allowed_acl_op(op))
					   ret = true;
				   break;
			   }
		   }
		   return ret;
	   }
	   inline bool is_owner(std::string &group_id)
	   {
		 bool ret = false;

		 for(auto t = acls.begin(); t != acls.end(); ++t)
		 {
			 if(t->get_group_id().compare(group_id)==0)
			 {
				 ret = true;
				 break;
			 }
		 }
		 return ret;
	   }
	   inline bool replace_acl(std::string &group_id,std::string &perm)
	   {
		bool ret = false;

		for(auto t = acls.begin(); t != acls.end(); ++t)
		{
			if(t->get_group_id().compare(group_id)==0)
			{
			    t->modify_acl(perm);    
			    ret = true;
			    break;
			}
		}
		return ret;

	   }

};

#endif
