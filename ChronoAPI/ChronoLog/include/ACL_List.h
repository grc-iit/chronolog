#ifndef ACL_LIST_H
#define ACL_LIST_H
#include "ACL.h"
#include "city.h"
#include <unordered_map>
#include <mutex>
#include <json-c/json.h>
#include <json-c/json_c_version.h>
#include <iostream>

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
	inline void add_entry(std::string &s,ACL_Map &m)
	{
		auto acl_record = acls->find(s);
		if(acl_record != acls->end())
		{
		   bool exists = false;
		   for(auto t = acl_record->second.begin(); t != acl_record->second.end();++t)
		   {
			if(t->get_group_id().compare(m.get_group_id())==0)
			{
			   exists = true; break;
			}
		   }  

		   if(!exists)
		   {
			acl_record->second.push_back(m);
		   }
		}
		else
		{
			std::vector<ACL_Map> nl;
			nl.push_back(m);
			std::pair<std::string,std::vector<ACL_Map>> p(s,nl);
			acls->insert(p);
		}

	}

	inline bool record_exists(std::string &name,std::string &group_id)
	{
	   bool ret = false;

	   auto acl_record = acls->find(name);

	   if(acl_record != acls->end())
	   {
		for(auto t = acl_record->second.begin(); t != acl_record->second.end(); ++t)
		{
			if(t->get_group_id().compare(group_id)==0)
			{
				ret = true; break;
			}
		}
	   }
	   return ret;
	}
	inline bool is_allowed(std::string &name,std::string &group_id,enum ChronoLogOp &op)
        {
	      bool ret = false;
	      auto acl_record = acls->find(name);

	      if(acl_record != acls->end())
	      {
                   for(auto t=acl_record->second.begin(); t != acl_record->second.end(); ++t)
                   {
                           if(t->get_group_id().compare(group_id)==0)
                           {
                                   if(t->is_allowed_acl_op(op))
                                           ret = true;
                                   break;
                           }
                   }
	      }
              return ret;
         }



        ~ACL_DB()
	{
	    delete acls;
	}

};

ACL_DB* CreateACLDB(std::string &filename);


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
