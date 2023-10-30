#include "MessageCache.h"

bool message_cache::add_message(std::string &msg)
{
   char ts_c[8];
   for(int i=0;i<8;i++)
     ts_c[i] = msg[i];

   uint64_t ts = *(uint64_t*)(ts_c);
 
   uint32_t prev = msg_count.fetch_add(1);
   prev = prev%num_messages;
  
   (*locks)[prev].lock();
   messages[prev].assign(msg.begin(),msg.end());
   (*locks)[prev].unlock(); 

   return true;
}
