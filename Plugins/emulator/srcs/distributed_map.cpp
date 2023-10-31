
template <typename KeyT,typename ValueT,typename HashFcn,typename EqualFcn>
void distributed_hashmap<KeyT,ValueT,HashFcn,EqualFcn>::create_table(uint64_t n,KeyT maxKey)
{
          uint64_t tsize = n;
          KeyT emptyKey = maxKey;
          assert (tsize > 0 && tsize < UINT64_MAX);
          uint64_t localSize = tsize/nservers;
          uint64_t rem = tsize%nservers;
          uint64_t maxSize;
          if(serverid < rem) maxSize = localSize+1;
          else maxSize = localSize;
          assert (maxSize > 0 && maxSize < UINT64_MAX);

          pool_type *pl = new pool_type(200);
          map_type *my_table = new map_type(maxSize,pl,emptyKey);
          int pv = num_tables.fetch_add(1);
          if(pv < maxtables)
          {
                totalSizes[pv] = tsize;
                maxSizes[pv] = maxSize;
                emptyKeys[pv] = emptyKey;
                my_tables[pv] = my_table;
                pls[pv] = pl;
                range[pv].first = 0;
                range[pv].second = UINT64_MAX;
          }

}

template <typename KeyT,typename ValueT,typename HashFcn,typename EqualFcn>
void distributed_hashmap<KeyT,ValueT,HashFcn,EqualFcn>::remove_table(int index)
{
        delete my_tables[index];
        delete pls[index];
        my_tables[index] = nullptr;
        pls[index] = nullptr;
}

template<typename KeyT,typename ValueT,typename HashFcn,typename EqualFcn>
uint64_t distributed_hashmap<KeyT,ValueT,HashFcn,EqualFcn>::serverLocation(KeyT &k,int i)
{
      uint64_t localSize = totalSizes[i]/nservers;
      uint64_t rem = totalSizes[i]%nservers;
      uint64_t hashval = HashFcn()(k);
      uint64_t v = hashval % totalSizes[i];
      uint64_t offset = rem*(localSize+1);
      uint64_t id = -1;
      if(v >= 0 && v < totalSizes[i])
      {
         if(v < offset)
           id = v/(localSize+1);
         else id = rem+((v-offset)/localSize);
      }

      return id;
}


