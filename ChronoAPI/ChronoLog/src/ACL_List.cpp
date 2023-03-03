#include "../include/ACL_List.h"
#include <iostream>


ACL_DB* CreateACLDB(std::string &json_filename)
{

   ACL_DB * acldb = new ACL_DB();
   struct json_object *config = nullptr;


   config = json_object_from_file(json_filename.c_str());
   struct json_object *clients_vec = json_object_object_get(config,"client");
   int num_clients = json_object_array_length(clients_vec);

   for(int i=0;i<num_clients;i++)
   {
	struct json_object *c = json_object_array_get_idx(clients_vec,i);

	std::string name(json_object_get_string(json_object_object_get(c, "name")));
	std::string ipaddress(json_object_get_string(json_object_object_get(c,"ipaddress")));
	std::string group(json_object_get_string(json_object_object_get(c,"group")));
	struct json_object *chron_vec = json_object_object_get(c,"chronicle");
	int num_chronicles = 0;
	if(chron_vec != NULL)
	  num_chronicles = json_object_array_length(chron_vec);

	for(int j=0;j<num_chronicles;j++)
	{
		struct json_object *chron = json_object_array_get_idx(chron_vec,j);
		std::string chronicle_name(json_object_get_string(json_object_object_get(chron,"chronicle_name")));
		std::string permission(json_object_get_string(json_object_object_get(chron,"permission")));
		ACL a; a.set_permission(permission);
		ACL_Map m;
		m.create_acl_map(group,a);
		acldb->add_entry(chronicle_name,m);
	}

	struct json_object *story_vec = json_object_object_get(c,"story");
	int num_stories = 0;
	if(story_vec != NULL)
		num_stories = json_object_array_length(story_vec);

	for(int j=0;j<num_stories;j++)
	{
		struct json_object *story = json_object_array_get_idx(story_vec,j);
		std::string chronicle_name(json_object_get_string(json_object_object_get(story,"chronicle_name")));
		std::string story_name(json_object_get_string(json_object_object_get(story,"story_name")));
		std::string permission(json_object_get_string(json_object_object_get(story,"permission")));
		ACL_Map m; ACL a; a.set_permission(permission);
		m.create_acl_map(group,a);
		std::string story_name_full = chronicle_name+story_name;
		acldb->add_entry(story_name_full,m);
	}
   }

    return acldb;
}	
