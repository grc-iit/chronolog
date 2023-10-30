#include "event.h"
#include <string>


std::string pack_event(struct event* e,int length)
{
   char tss[8];
   char *tsstring = (char*)(&e->ts);
   for(int i=0;i<8;i++)
     tss[i] = tsstring[i];
   std::string s(tss,8);
   s += std::string(e->data,length);
   return s;
}


void unpack_event(struct event*e,std::string &s)
{
   int length = sizeof(uint64_t);
   char tstring[length]; 
   for(int i=0;i<length;i++)
   {
	tstring[i] = s[i];
   }

   e->ts = *(uint64_t*)tstring;
   for(int i=sizeof(uint64_t);i<s.length();i++)
   e->data[i] = s[i];
}




