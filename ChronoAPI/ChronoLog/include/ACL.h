#ifndef CHRONOLOG_ACL_H
#define CHRONOLOG_ACL_H


class ACL
{
  private:
        uint32_t permission;
  public:
	ACL(){ permission = 1;}	
	~ACL(){};

	inline void set_permission(uint32_t p)
	{
		permission = p;
	}
	inline void set_permission(std::string &perm)
	{
		uint32_t p = 1;
		if(perm.compare("RWCD")==0)
		{
			p = 15;
		}
		else if(perm.compare("RW")==0)
		{
			p = 7;
		}
		else if(perm.compare("RONLY")==0)
		{
			p = 3;
		}
		permission = p;
	}
	inline uint32_t get_permission()
	{
		return permission;
	}
	inline bool get_op_permission(enum ChronoLogOp &op)
	{
	     uint32_t mask = 1;
	     mask = mask << (uint32_t)op;
	     uint32_t v = mask & permission;
	     if(v==0) return false;
	     else return true;
	}
	inline void set_op_permission(enum ChronoLogOp &op)
	{
	      uint32_t mask = 1;
	      mask = mask << (uint32_t)op;
	      permission = permission | mask;
	}
	inline void unset_op_permission(enum ChronoLogOp &op)
	{
	     uint32_t mask1 = UINT32_MAX;
	     uint32_t mask2 = 1; mask2 = mask2 << (uint32_t)op;
	     mask1 = mask1 ^ mask2;
	     permission = permission & mask1;
	     mask2 = 1;
	     permission = permission | mask2;
	}

};

class ACL_Map
{
  
  private:
        std::pair<std::string,ACL> binding;
   
  public:
	ACL_Map(){};
	~ACL_Map(){};

	inline void create_acl_map(std::string &s, ACL &a)
	{
		binding.first = s;
		binding.second = a;
	}
	inline std::string& get_group_id()
	{
	     return binding.first;
	}
	inline ACL & get_acl()
	{
		return binding.second;
	}
	inline void modify_acl(ACL &a)
	{
		binding.second = a;
	}
	inline void modify_acl(std::string &p)
	{
		binding.second.set_permission(p);
	}
	inline bool is_allowed_acl_op(enum ChronoLogOp &op)
	{
		return binding.second.get_op_permission(op);
	}
	inline void allow_acl_op(enum ChronoLogOp &op)
	{
		binding.second.set_op_permission(op);
	}
	inline void deny_acl_op(enum ChronoLogOp &op)
	{
	        binding.second.unset_op_permission(op);
	}
};

#endif
