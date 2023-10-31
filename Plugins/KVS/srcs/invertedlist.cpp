
template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
int hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::partition_no(KeyT &k)
{

      uint64_t hashval = hashfcn()(k);
      uint64_t key = hashval;
      uint64_t mask = UINT64_MAX;
      mask = mask << (64-nbits);
      mask = mask >> (64-nbits);
      key = key & mask;
      key = key >> (nbits-nbits_p);   
      int id = (int)(key);
      int pid = id%numprocs;
      /*int rem = ntables%numprocs;
      int offset = rem*(tables_per_proc+1);
      int pid = -1;
      if(id < offset)
      {
	pid = id/(tables_per_proc+1);
      }  
      else
      {
	id = id-offset;
	pid = rem+(id/tables_per_proc);
      }*/
      return pid;
}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
bool hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::put_entry(KeyT &k,ValueT &v)
{
	int pid = partition_no(k);

	assert (pid >= 0 && pid < numprocs);

	bool b = false;
	
	if(pid == serverid)
	   b = LocalPutEntry(k,v);
	else
	   b = PutEntry(k,v,pid);
	return b;
}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
int hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::get_entry(KeyT& k,std::vector<ValueT>&values)
{
	int pid = partition_no(k);
	assert (pid >= 0 && pid < numprocs);
        int ret = -1;
	if(pid==serverid)
	   values = LocalGetEntry(k);
	else values = GetEntry(k,pid);
	if(values.size()>0) ret = pid;

	return ret;
}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::get_events()
{

  int exists_f = index_writes > 0 ? 1 : 0;
  int exists_t = 0;
  MPI_Allreduce(&exists_f,&exists_t,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
  if(exists_t == numprocs)
  {
   
  std::string indexname = dir;
  indexname += "file"+filename+"index"+".h5";
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);

  H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL);
  H5Pset_dxpl_mpio(dxpl, H5FD_MPIO_COLLECTIVE);

  hid_t xfer_plist = H5Pcreate(H5P_DATASET_XFER);
  H5Pset_dxpl_mpio(xfer_plist,H5FD_MPIO_INDEPENDENT);

  hsize_t maxdims[1];
  maxdims[0] = (hsize_t)H5S_UNLIMITED;

  int v_i;float v_f;double v_d;unsigned long v_l;
  hid_t datatype;
  KeyT key;
  if(typeid(key)==typeid(v_i)) datatype = H5Tcopy(H5T_NATIVE_INT);
  else if(typeid(key)==typeid(v_f)) datatype = H5Tcopy(H5T_NATIVE_FLOAT);
  else if(typeid(key)==typeid(v_d)) datatype = H5Tcopy(H5T_NATIVE_DOUBLE);
  else if(typeid(key)==typeid(v_l)) datatype = H5Tcopy(H5T_NATIVE_UINT64);

  hid_t kv1 = H5Tcreate(H5T_COMPOUND,sizeof(struct KeyIndex<KeyT>));
  H5Tinsert(kv1,"key",HOFFSET(struct KeyIndex<KeyT>,key),datatype);
  H5Tinsert(kv1,"index",HOFFSET(struct KeyIndex<KeyT>,index),H5T_NATIVE_UINT64);

  hid_t fid = H5Fopen(indexname.c_str(),H5F_ACC_RDONLY,fapl);

  std::string d_string = attributename;
  std::string dtable = attributename+"attr";
  hid_t dataset_t = H5Dopen(fid,dtable.c_str(),H5P_DEFAULT);
  hid_t file_dataspace_t = H5Dget_space(dataset_t);


  hsize_t toffset = pre_table*2*pow(2,nbits_r);

  hsize_t lsize = 2*maxsize;

  cached_keyindex_mt.resize(2*maxsize);

  int ret = H5Sselect_hyperslab(file_dataspace_t,H5S_SELECT_SET,&toffset,NULL,&lsize,NULL);
  hid_t mem_dataspace_t = H5Screate_simple(1,&lsize,NULL);
  ret = H5Dread(dataset_t,H5T_NATIVE_INT,mem_dataspace_t,file_dataspace_t,dxpl,cached_keyindex_mt.data());
  H5Sclose(mem_dataspace_t);

  hid_t dataset_k = H5Dopen(fid,d_string.c_str(),H5P_DEFAULT);

  hid_t file_dataspace = H5Dget_space(dataset_k);
 
  std::vector<struct event_req<KeyT,ValueT>*> worklist1;
     
  while(!pending_gets->empty())
  {
        struct event_req<KeyT,ValueT> *r;
        if(pending_gets->pop(r))
        {
          KeyT key = r->key;
          uint64_t hashvalue = hashfcn()(key);
          int pid = partition_no(key);
          assert (pid == myrank);
          uint64_t pos = hashvalue%maxsize;
          hsize_t offset = cached_keyindex_mt[2*pos+1];
          hsize_t numkeys = cached_keyindex_mt[2*pos];
      
          std::vector<struct KeyIndex<KeyT>> keyindex;
          keyindex.resize(numkeys);
          ret = H5Sselect_hyperslab(file_dataspace,H5S_SELECT_SET,&offset,NULL,&numkeys,NULL);
          hid_t mem_dataspace = H5Screate_simple(1,&numkeys,NULL);
          ret = H5Dread(dataset_k,kv1,mem_dataspace,file_dataspace,xfer_plist,keyindex.data());
          H5Sclose(mem_dataspace);

          uint64_t ts = UINT64_MAX;

          for(int k=0;k<keyindex.size();k++)
	  if(keyindex[k].key==key)
	  {
		ts = keyindex[k].index;
		break;
	  }
          if(ts != UINT64_MAX) 
          {
	     std::string eventstring = if_q->GetEmulatorEvent(filename,ts,myrank);
	     if(eventstring.length() != 0)
	     {
	        ost << eventstring << std::endl;
	     }
          }
          delete r;
	}
     }

     H5Sclose(file_dataspace);
     H5Dclose(dataset_k);
     H5Sclose(file_dataspace_t);
     H5Dclose(dataset_t);
     H5Tclose(kv1);
     H5Pclose(xfer_plist);
     H5Pclose(fapl);
     H5Pclose(dxpl);
     H5Fclose(fid);
  }  
   
}

template<typename KeyT, typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::fill_invlist_from_file(std::string &s,int offset)
{
   std::string fname = dir;
   fname += "file";
   fname += filename+".h5";

   hid_t xfer_plist = H5Pcreate(H5P_DATASET_XFER);
   hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
   H5Pset_fapl_mpio(fapl,MPI_COMM_WORLD, MPI_INFO_NULL);
   H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

   hsize_t chunkdims[1];
   chunkdims[0] = 8192;
   hsize_t maxdims[1];
   maxdims[0] = (hsize_t)H5S_UNLIMITED;

   hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);

   int ret = H5Pset_chunk(dataset_pl,1,chunkdims);

   hid_t fid = H5Fopen(fname.c_str(),H5F_ACC_RDWR,fapl);     

   hsize_t attr_size[1];
   attr_size[0] = MAXBLOCKS*4+4;
   const char *attrname[1];
   hid_t attr_space[1];
   attr_space[0] = H5Screate_simple(1, attr_size, NULL);

   attrname[0] = "Datasizes";

   std::string data_string = "Data1";
   hid_t dataset1 = H5Dopen2(fid,data_string.c_str(), H5P_DEFAULT);

   hid_t attr_id = H5Aopen(dataset1,attrname[0],H5P_DEFAULT);
   std::vector<uint64_t> attrs;
   attrs.resize(attr_size[0]);

   ret = H5Aread(attr_id,H5T_NATIVE_UINT64,attrs.data());

    hsize_t adims[1];
    adims[0] = VALUESIZE;
    hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR,1,adims);
    hid_t s2 = H5Tcreate(H5T_COMPOUND,sizeof(struct event));
    H5Tinsert(s2,"key",HOFFSET(struct event,ts),H5T_NATIVE_UINT64);
    H5Tinsert(s2,"value",HOFFSET(struct event,data),s1);

    int numblocks = attrs[3];


    if(myrank==0) std::cout <<" numblocks = "<<numblocks<<std::endl;

    hid_t file_dataspace = H5Dget_space(dataset1);

    std::vector<struct event> *buffer = new std::vector<struct event> ();

    int pos = 4;
    hsize_t offset_r = 0;
    for(int i=0;i<numblocks;i++)
    {
	int nrecords = attrs[pos+i*4+3];

	int records_per_proc = nrecords/numprocs;
	int rem = nrecords%numprocs;

	hsize_t pre = offset_r;
	for(int i=0;i<myrank;i++)
	{	
	   int size_p=0;
	   if(i < rem) size_p = records_per_proc+1;
	   else size_p = records_per_proc;
	   pre += size_p;
	}

	hsize_t blocksize;
        if(myrank < rem) blocksize = records_per_proc+1;
 	else blocksize = records_per_proc;	
	
	buffer->clear();
	buffer->resize(blocksize);

        ret = H5Sselect_hyperslab(file_dataspace, H5S_SELECT_SET,&pre,NULL,&blocksize,NULL);
        hid_t mem_dataspace = H5Screate_simple(1,&blocksize, NULL);
        ret = H5Dread(dataset1,s2, mem_dataspace, file_dataspace, xfer_plist,buffer->data());
	H5Sclose(mem_dataspace);

	add_entries_to_tables(s,buffer,pre,offset);

	offset_r += nrecords;
    }
    
  delete buffer;
  H5Tclose(s2);
  H5Tclose(s1);
  H5Sclose(file_dataspace);
  H5Sclose(attr_space[0]);
  H5Dclose(dataset1);
  H5Aclose(attr_id);
  H5Fclose(fid);

}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::flush_timestamps(int offset,bool persist)
{

 int send_v = file_exists.load() == true ? 1 : 0;
 int send_v_t = 0;
 MPI_Allreduce(&send_v,&send_v_t,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

 if(send_v_t != numprocs) return;

 std::vector<struct KeyIndex<KeyT>> KeyTimestamps;
 uint64_t maxts = UINT64_MAX;
 int key_pre = 0;
 int total_keys = 0;
 get_entries_from_tables(KeyTimestamps,key_pre,total_keys,maxts);

 std::vector<struct KeyIndex<KeyT>> preKeyTimestamps;

 if(index_writes == 0)
 {
    cached_keyindex_mt.resize(2*maxsize);
    std::fill(cached_keyindex_mt.begin(),cached_keyindex_mt.end(),0);
    int offset_t = pre_table*2*pow(2,nbits_r);
    int prev_total = 0;
    std::vector<int> numkeys_n(2*maxsize);
    std::fill(numkeys_n.begin(),numkeys_n.end(),0);
   
    std::vector<struct KeyIndex<KeyT>> mergeKeyTimestamps = merge_keyoffsets(preKeyTimestamps,KeyTimestamps,numkeys_n);
    
    cached_keyindex.clear();
    cached_keyindex.resize(mergeKeyTimestamps.size());
    cached_keyindex.assign(mergeKeyTimestamps.begin(),mergeKeyTimestamps.end());

    cached_keyindex_mt.clear();
    cached_keyindex_mt.resize(2*maxsize);
    cached_keyindex_mt.assign(numkeys_n.begin(),numkeys_n.end());

    flush_count++;
    create_index_file();
 } 
 else
 {
   std::string indexname = dir;
   indexname += "file"+filename+"index"+".h5";
   hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
   hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);

   H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL);
   H5Pset_dxpl_mpio(dxpl, H5FD_MPIO_COLLECTIVE);

   hsize_t maxdims[1];
   maxdims[0] = (hsize_t)H5S_UNLIMITED;

   int v_i;float v_f;double v_d;unsigned long v_l;
   hid_t datatype;
   KeyT key;
   if(typeid(key)==typeid(v_i)) datatype = H5Tcopy(H5T_NATIVE_INT);
   else if(typeid(key)==typeid(v_f)) datatype = H5Tcopy(H5T_NATIVE_FLOAT);
   else if(typeid(key)==typeid(v_d)) datatype = H5Tcopy(H5T_NATIVE_DOUBLE);
   else if(typeid(key)==typeid(v_l)) datatype = H5Tcopy(H5T_NATIVE_UINT64);

   hid_t kv1 = H5Tcreate(H5T_COMPOUND,sizeof(struct KeyIndex<KeyT>));
   H5Tinsert(kv1,"key",HOFFSET(struct KeyIndex<KeyT>,key),datatype);
   H5Tinsert(kv1,"index",HOFFSET(struct KeyIndex<KeyT>,index),H5T_NATIVE_UINT64);

   hid_t fid = H5Fopen(indexname.c_str(),H5F_ACC_RDONLY,fapl);

   std::string d_string = attributename;
   std::string dtable = attributename+"attr";
   hid_t dataset_t = H5Dopen(fid,dtable.c_str(),H5P_DEFAULT);
   hid_t file_dataspace_t = H5Dget_space(dataset_t);


   hsize_t toffset = pre_table*2*pow(2,nbits_r);

   hsize_t lsize = 2*maxsize;

   cached_keyindex_mt.resize(2*maxsize);

   int ret = H5Sselect_hyperslab(file_dataspace_t,H5S_SELECT_SET,&toffset,NULL,&lsize,NULL);
   hid_t mem_dataspace_t = H5Screate_simple(1,&lsize,NULL);
   ret = H5Dread(dataset_t,H5T_NATIVE_INT,mem_dataspace_t,file_dataspace_t,dxpl,cached_keyindex_mt.data());
   H5Sclose(mem_dataspace_t);

   int total_keys_l = 0;
   for(int i=0;i<maxsize;i++) total_keys_l += cached_keyindex_mt[2*i];

   cached_keyindex.resize(total_keys_l);

   int total_keys = 0;
   MPI_Allreduce(&total_keys_l,&total_keys,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

   hid_t dataset_k = H5Dopen(fid,d_string.c_str(),H5P_DEFAULT);

   hid_t file_dataspace = H5Dget_space(dataset_k);

   hsize_t offset_w = cached_keyindex_mt[1];
   hsize_t blocksize = (hsize_t)total_keys_l;

   hid_t mem_dataspace = H5Screate_simple(1,&blocksize,NULL);
   ret = H5Sselect_hyperslab(file_dataspace,H5S_SELECT_SET,&offset_w,NULL,&blocksize,NULL);
   ret = H5Dread(dataset_k,kv1,mem_dataspace,file_dataspace,dxpl,cached_keyindex.data());
   H5Sclose(mem_dataspace);



   H5Sclose(file_dataspace);
   H5Dclose(dataset_k);
   H5Sclose(file_dataspace_t);
   H5Dclose(dataset_t);
   H5Tclose(kv1);
   H5Pclose(fapl);
   H5Pclose(dxpl);
   H5Fclose(fid);

   std::vector<int> numkeys_n(2*maxsize);
   std::fill(numkeys_n.begin(),numkeys_n.end(),0);
   preKeyTimestamps.assign(cached_keyindex.begin(),cached_keyindex.end());

   std::vector<struct KeyIndex<KeyT>> mergeKeyTimestamps = merge_keyoffsets(preKeyTimestamps,KeyTimestamps,numkeys_n);

    cached_keyindex.clear();
    cached_keyindex.resize(mergeKeyTimestamps.size());
    cached_keyindex.assign(mergeKeyTimestamps.begin(),mergeKeyTimestamps.end());

    cached_keyindex_mt.clear();
    cached_keyindex_mt.resize(2*maxsize);
    cached_keyindex_mt.assign(numkeys_n.begin(),numkeys_n.end());
    flush_count++;
    update_index_file();

 }

}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::flush_table_file(int offset,bool persist)
{
 std::string fname = dir;
 fname+="file";
 fname += filename+".h5";

 {
 int send_v = file_exists.load()==true ? 1 : 0;
 int sum_v = 0;

 MPI_Allreduce(&send_v,&sum_v,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

 if(sum_v == numprocs)
 {
  hid_t xfer_plist = H5Pcreate(H5P_DATASET_XFER);
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  H5Pset_fapl_mpio(fapl,MPI_COMM_WORLD, MPI_INFO_NULL);
  H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

  hid_t xfer_plist2 = H5Pcreate(H5P_DATASET_XFER);
  H5Pset_dxpl_mpio(xfer_plist2,H5FD_MPIO_INDEPENDENT);

 hsize_t chunkdims[1];
 chunkdims[0] = 8192;
 hsize_t maxdims[1];
 maxdims[0] = (hsize_t)H5S_UNLIMITED;

 hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);

 int ret = H5Pset_chunk(dataset_pl,1,chunkdims);

 hid_t fid = H5Fopen(fname.c_str(),H5F_ACC_RDONLY,fapl);

 hsize_t adims[1];
 adims[0] = datasize;
 int keydatasize = sizeof(uint64_t)+datasize;
 hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR,1,adims);
 hid_t s2 = H5Tcreate(H5T_COMPOUND,keydatasize);
 H5Tinsert(s2,"key",HOFFSET(struct keydata,ts),H5T_NATIVE_UINT64);
 H5Tinsert(s2,"value",HOFFSET(struct keydata,data),s1);
 hsize_t attr_size[1];
 attr_size[0] = MAXBLOCKS*4+4;
 const char *attrname[1];

 attrname[0] = "Datasizes";
  
 std::string data_string = "Data1";
 hid_t dataset1 = H5Dopen(fid,data_string.c_str(), H5P_DEFAULT);

 hid_t file_dataspace = H5Dget_space(dataset1);
 hid_t attr_id = H5Aopen(dataset1,attrname[0],H5P_DEFAULT);
 std::vector<uint64_t> attrs;
 attrs.resize(attr_size[0]);

 ret = H5Aread(attr_id,H5T_NATIVE_UINT64,attrs.data());
 
 std::vector<std::vector<KeyT>> *keys = new std::vector<std::vector<KeyT>> ();
 std::vector<std::vector<ValueT>> *timestamps = new std::vector<std::vector<ValueT>> ();

 int numblocks = attrs[3];
 int pos = 4;

 uint64_t maxts = attrs[pos+(numblocks-1)*4+1];
 if(myrank==0) std::cout <<" total records = "<<attrs[0]<<" maxts = "<<maxts<<std::endl;

 int key_pre = 0;
 int total_keys = 0;

 std::vector<struct KeyIndex<KeyT>> KeyTimestamps;

 get_entries_from_tables(KeyTimestamps,key_pre,total_keys,maxts); 

 std::vector<struct KeyIndex<int>> Timestamp_order;

 for(int i=0;i<KeyTimestamps.size();i++)
 {
   struct KeyIndex<int> pt;
   pt.key = i;
   pt.index = KeyTimestamps[i].index;
   Timestamp_order.push_back(pt);
 }

 std::sort(Timestamp_order.begin(),Timestamp_order.end(),compareIndex<int>);


 int block_id = 0;
 std::vector<int> block_ids;
 int i=0;

 int min_block = INT_MAX;
 int max_block = INT_MIN;

 std::vector<std::vector<int>> block_index;

 block_index.resize(numblocks);

 block_ids.resize(Timestamp_order.size());
 std::fill(block_ids.begin(),block_ids.end(),0);

 
 while(i < Timestamp_order.size())
 {
   if(block_id >= numblocks) break;
   uint64_t ts = Timestamp_order[i].index; 
   uint64_t minkey = attrs[pos+block_id*4+0];
   uint64_t maxkey = attrs[pos+block_id*4+1];
   if(ts >= minkey && ts <= maxkey) 
   {
	block_ids.push_back(block_id); 
	block_index[block_id].push_back(i);
	i++;
	if(min_block > block_id) min_block = block_id;
	if(max_block < block_id) max_block = block_id;
   }
   else if(ts < minkey) 
   {
	   block_ids[Timestamp_order[i].key] = -1;
	   i++;
   }
   else if(ts > maxkey) block_id++;
 }


 while(i < Timestamp_order.size())
 {
    block_ids[Timestamp_order[i].key] = -1;
    i++;
 }


 std::vector<char> *buffer = new std::vector<char> ();

 for(int i=0;i<block_index.size();i++)
 {
    if(block_index[i].size() > 0)
    {
    block_id = i;

    int nrecords = attrs[pos+block_id*4+3];

    hsize_t pre = 0;
    for(int k=0;k<block_id;k++)
    {
           pre += attrs[pos+k*4+3];
    }
    
    hsize_t blocksize = nrecords;
        
    buffer->clear();
    buffer->resize(blocksize*keydatasize);

    ret = H5Sselect_hyperslab(file_dataspace, H5S_SELECT_SET,&pre,NULL,&blocksize,NULL);
    hid_t mem_dataspace = H5Screate_simple(1,&blocksize, NULL);
    ret = H5Dread(dataset1,s2, mem_dataspace, file_dataspace, xfer_plist2,buffer->data());
    H5Sclose(mem_dataspace);

    int k = 0;
    int r = 0;
    for(int j=0;j<block_index[i].size();j++)
    {
	int p = block_index[i][j];
	if(k < buffer->size())
	{
          uint64_t ts = *(uint64_t*)(&((*buffer)[k]));
	  while(Timestamp_order[p].index > ts)
	  {
		  k+=keydatasize;
		  r++;
		  if(k==buffer->size()) break;
		  ts = *(uint64_t*)(&((*buffer)[k]));
	  }
	  if(k==buffer->size()) break;
	  if(Timestamp_order[p].index == ts)
	  {
	   Timestamp_order[p].index = pre+r;
	  }
	  else if(Timestamp_order[p].index < ts)
	  {
		block_ids[Timestamp_order[p].key] = -1;

	  }
	}
    }
    }


 } 

 delete buffer;

 std::vector<struct KeyIndex<KeyT>> KeyTimestamps_s;

 for(int i=0;i<Timestamp_order.size();i++)
 {
	int p = Timestamp_order[i].key;
	KeyTimestamps[p].index = Timestamp_order[i].index;
 }

 for(int i=0;i<KeyTimestamps.size();i++)
 {
	if(block_ids[i] != -1)
	{
           KeyTimestamps_s.push_back(KeyTimestamps[i]);
	}
 }

 KeyTimestamps.clear(); block_ids.clear();

 std::vector<int> m_size_t(numprocs);
 std::vector<int> msizes(numprocs);

 std::fill(m_size_t.begin(),m_size_t.end(),0);
 std::fill(msizes.begin(),msizes.end(),0);
 m_size_t[myrank] = KeyTimestamps_s.size();

 MPI_Allreduce(m_size_t.data(),msizes.data(),numprocs,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

 total_keys = 0;
 for(int i=0;i<msizes.size();i++) total_keys += msizes[i];

 const char *attrname_k[1];
 std::string attr_t = attributename+"attr";
 attrname_k[0] = attr_t.c_str();

 if(myrank==0) std::cout <<" total_keys = "<<total_keys<<std::endl;

 if(cached_keyindex_mt.size()==0) 
 {
    cached_keyindex_mt.resize(2*maxsize);
    std::fill(cached_keyindex_mt.begin(),cached_keyindex_mt.end(),0);
 }

  int offset_t = pre_table*2*pow(2,nbits_r);

  int prev_keys = 0;
  for(int i=0;i<maxsize;i++) prev_keys += cached_keyindex_mt[2*i];

  int prev_total = 0;
  MPI_Allreduce(&prev_keys,&prev_total,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

  if(myrank==0) std::cout <<" prev total = "<<prev_total<<std::endl;

  std::vector<struct KeyIndex<KeyT>> preKeyTimestamps;

  if(prev_keys > 0) preKeyTimestamps.resize(prev_keys);

  preKeyTimestamps.assign(cached_keyindex.begin(),cached_keyindex.end());

  std::vector<int> numkeys_n(2*maxsize);
  std::fill(numkeys_n.begin(),numkeys_n.end(),0);

  std::vector<struct KeyIndex<KeyT>> mergeKeyTimestamps = merge_keyoffsets(preKeyTimestamps,KeyTimestamps_s,numkeys_n);

  cached_keyindex.clear();
  cached_keyindex.resize(mergeKeyTimestamps.size());
  cached_keyindex.assign(mergeKeyTimestamps.begin(),mergeKeyTimestamps.end());

  cached_keyindex_mt.clear();
  cached_keyindex_mt.resize(2*maxsize);
  cached_keyindex_mt.assign(numkeys_n.begin(),numkeys_n.end());

  flush_count++;
 MPI_Barrier(MPI_COMM_WORLD);

 H5Aclose(attr_id);
 H5Sclose(file_dataspace);
 H5Dclose(dataset1);
 H5Tclose(s2);
 H5Tclose(s1);
 H5Pclose(dataset_pl);
 H5Pclose(xfer_plist);
 H5Pclose(xfer_plist2);
 H5Pclose(fapl);
 H5Fclose(fid);

 if(index_writes==0) create_index_file();
 else update_index_file();
 }
 }

}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::update_index_file()
{
   std::string indexname = dir;
   indexname += "file"+filename+"index"+".h5";

   hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
   hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);

   H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL);
   H5Pset_dxpl_mpio(dxpl, H5FD_MPIO_COLLECTIVE);

    hsize_t maxdims[1];
    maxdims[0] = (hsize_t)H5S_UNLIMITED;

    int v_i;float v_f;double v_d;unsigned long v_l;
    hid_t datatype;
    KeyT key;
    if(typeid(key)==typeid(v_i)) datatype = H5Tcopy(H5T_NATIVE_INT);
    else if(typeid(key)==typeid(v_f)) datatype = H5Tcopy(H5T_NATIVE_FLOAT);
    else if(typeid(key)==typeid(v_d)) datatype = H5Tcopy(H5T_NATIVE_DOUBLE);
    else if(typeid(key)==typeid(v_l)) datatype = H5Tcopy(H5T_NATIVE_UINT64);

    hid_t kv1 = H5Tcreate(H5T_COMPOUND,sizeof(struct KeyIndex<KeyT>));
    H5Tinsert(kv1,"key",HOFFSET(struct KeyIndex<KeyT>,key),datatype);
    H5Tinsert(kv1,"index",HOFFSET(struct KeyIndex<KeyT>,index),H5T_NATIVE_UINT64);

    hid_t fid = H5Fopen(indexname.c_str(),H5F_ACC_RDWR,fapl);

    std::string d_string = attributename;
    std::string dtable = attributename+"attr";
    hid_t dataset_t = H5Dopen(fid,dtable.c_str(),H5P_DEFAULT);
    hid_t file_dataspace_t = H5Dget_space(dataset_t);

    int cache_size = cached_keyindex.size();
    int max_cache_size = 0;

    MPI_Allreduce(&cache_size,&max_cache_size,1,MPI_INT,MPI_MAX,MPI_COMM_WORLD);

    if(max_cache_size > 0)
    {
    hsize_t toffset = pre_table*2*pow(2,nbits_r);

    hsize_t lsize = 2*maxsize;

    int ret = H5Sselect_hyperslab(file_dataspace_t,H5S_SELECT_SET,&toffset,NULL,&lsize,NULL);
    hid_t mem_dataspace_t = H5Screate_simple(1,&lsize,NULL);
    ret = H5Dwrite(dataset_t,H5T_NATIVE_INT,mem_dataspace_t,file_dataspace_t,dxpl,cached_keyindex_mt.data());
    H5Sclose(mem_dataspace_t);
    
    int total_keys_l = 0;
    for(int i=0;i<maxsize;i++) total_keys_l += cached_keyindex_mt[2*i];

    int total_keys = 0;
    MPI_Allreduce(&total_keys_l,&total_keys,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

    hid_t dataset_k = H5Dopen(fid,d_string.c_str(),H5P_DEFAULT);

    hsize_t totalk = (hsize_t)total_keys;
    H5Dset_extent(dataset_k,&totalk);
    hid_t file_dataspace = H5Dget_space(dataset_k);

    hsize_t offset_w = cached_keyindex_mt[1];
    hsize_t blocksize = (hsize_t)total_keys_l;

    hid_t mem_dataspace = H5Screate_simple(1,&blocksize,NULL);
    ret = H5Sselect_hyperslab(file_dataspace,H5S_SELECT_SET,&offset_w,NULL,&blocksize,NULL);
    ret = H5Dwrite(dataset_k,kv1,mem_dataspace,file_dataspace,dxpl,cached_keyindex.data());
    H5Sclose(mem_dataspace);

    index_writes++;
    H5Sclose(file_dataspace);
    H5Dclose(dataset_k);
    }
    H5Sclose(file_dataspace_t);
    H5Dclose(dataset_t);
    H5Tclose(kv1);
    H5Pclose(fapl);
    H5Pclose(dxpl);
    H5Fclose(fid);

}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::create_index_file()
{
    std::string indexname = dir;
    indexname+="file"+filename+"index"+".h5";

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);

    H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL);
    H5Pset_dxpl_mpio(dxpl, H5FD_MPIO_COLLECTIVE);

    hsize_t chunkdims[1];
    chunkdims[0] = 8192;
    hsize_t maxdims[1];
    maxdims[0] = (hsize_t)H5S_UNLIMITED;

    int v_i;float v_f;double v_d;unsigned long v_l;
    hid_t datatype;
    KeyT key;
    if(typeid(key)==typeid(v_i)) datatype = H5Tcopy(H5T_NATIVE_INT);
    else if(typeid(key)==typeid(v_f)) datatype = H5Tcopy(H5T_NATIVE_FLOAT);
    else if(typeid(key)==typeid(v_d)) datatype = H5Tcopy(H5T_NATIVE_DOUBLE);
    else if(typeid(key)==typeid(v_l)) datatype = H5Tcopy(H5T_NATIVE_UINT64);

    hid_t kv1 = H5Tcreate(H5T_COMPOUND,sizeof(struct KeyIndex<KeyT>));
    H5Tinsert(kv1,"key",HOFFSET(struct KeyIndex<KeyT>,key),datatype);
    H5Tinsert(kv1,"index",HOFFSET(struct KeyIndex<KeyT>,index),H5T_NATIVE_UINT64);


    hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);
    int ret = H5Pset_chunk(dataset_pl,1,chunkdims);
    hid_t fid = H5Fcreate(indexname.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,fapl);

    hsize_t totalt = 2*totalsize; 
    hid_t file_dataspace_t = H5Screate_simple(1,&totalt,maxdims);

    std::string d_string = attributename;
    std::string dtable = attributename+"attr";
    hid_t dataset_t = H5Dcreate(fid,dtable.c_str(),H5T_NATIVE_INT,file_dataspace_t,H5P_DEFAULT,dataset_pl,H5P_DEFAULT);

    hsize_t toffset = pre_table*2*pow(2,nbits_r);

     hsize_t lsize = 2*maxsize;
     ret = H5Sselect_hyperslab(file_dataspace_t,H5S_SELECT_SET,&toffset,NULL,&lsize,NULL);
     hid_t mem_dataspace_t = H5Screate_simple(1,&lsize,NULL);
     ret = H5Dwrite(dataset_t,H5T_NATIVE_INT,mem_dataspace_t,file_dataspace_t,dxpl,cached_keyindex_mt.data());
     H5Sclose(mem_dataspace_t);

     int totalkeys_l = 0;
     for(int i=0;i<maxsize;i++) totalkeys_l += cached_keyindex_mt[2*i];

     int totalkeys = 0;
     MPI_Allreduce(&totalkeys_l,&totalkeys,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

     hsize_t tkeys = (hsize_t)totalkeys;
     hid_t file_dataspace = H5Screate_simple(1,&tkeys,maxdims);
     hid_t dataset_k = H5Dcreate(fid,d_string.c_str(),kv1,file_dataspace,H5P_DEFAULT,dataset_pl,H5P_DEFAULT);

     hsize_t offset_w = cached_keyindex_mt[1];
     hsize_t blocksize = totalkeys_l;

     hid_t mem_dataspace = H5Screate_simple(1,&blocksize,NULL);

     ret = H5Sselect_hyperslab(file_dataspace,H5S_SELECT_SET,&offset_w,NULL,&blocksize,NULL);
     ret = H5Dwrite(dataset_k,kv1,mem_dataspace,file_dataspace,dxpl,cached_keyindex.data());
     H5Sclose(mem_dataspace);
    
     index_writes++; 

    H5Sclose(file_dataspace_t);
    H5Dclose(dataset_t);
    H5Sclose(file_dataspace);
    H5Dclose(dataset_k);
    H5Tclose(kv1);
    H5Pclose(dataset_pl);
    H5Pclose(fapl);
    H5Pclose(dxpl);
    H5Fclose(fid);

}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
std::vector<struct KeyIndex<KeyT>> hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::merge_keyoffsets(std::vector<struct KeyIndex<KeyT>> &prevlist,std::vector<struct KeyIndex<KeyT>> &newlist,std::vector<int> &numkeys)
{
 
   std::vector<struct KeyIndex<KeyT>> result_list;

   int i=0,j=0;

   while(true)
   {
     if(i==prevlist.size()||j==newlist.size()) break;

     uint64_t key1 = hashfcn()(prevlist[i].key);
     uint64_t key2 = hashfcn()(newlist[j].key);
     int pos1 = key1%maxsize;
     int pos2 = key2%maxsize;

     if(pos1 < pos2)
     {
	result_list.push_back(prevlist[i]);
	numkeys[2*pos1]++;
	i++;
     }
     else if(pos2 < pos1)
     {
	result_list.push_back(newlist[j]);
	numkeys[2*pos2]++;
	j++;
     }
     else
     {
	if(key1 < key2)
	{
	    numkeys[2*pos1]+=2;
	    result_list.push_back(prevlist[i]);
	    i++;
	    result_list.push_back(newlist[j]);
	    j++;
	}
	else
	{
	    numkeys[2*pos1]+=2;
	    result_list.push_back(newlist[j]);
	    j++;
	    result_list.push_back(prevlist[i]);
	    i++;
	}

     }

   }

   while(j<newlist.size())
   {
	uint64_t key1 = hashfcn()(newlist[j].key);
	int pos1 = key1%maxsize;
	result_list.push_back(newlist[j]);
	numkeys[2*pos1]++;
	j++;
   }

   while(i<prevlist.size())
   {
   	   uint64_t key2 = hashfcn()(prevlist[i].key);
	   int pos2 = key2%maxsize;	   
	   result_list.push_back(prevlist[i]);
	   numkeys[2*pos2]++;
	   i++;
   }

   std::vector<int> recv_size(numprocs);
   std::fill(recv_size.begin(),recv_size.end(),0);

   std::vector<int> send_size(numprocs);
   std::fill(send_size.begin(),send_size.end(),0);

   send_size[myrank] = result_list.size();

   MPI_Allreduce(send_size.data(),recv_size.data(),numprocs,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

   int prefix = 0;
   for(int i=0;i<myrank;i++) prefix += recv_size[i];

   numkeys[1] = prefix;
   for(int i=1;i<maxsize;i++) 
     numkeys[2*i+1] = numkeys[2*(i-1)+1]+numkeys[2*(i-1)];

   return result_list;
}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::add_entries_to_tables(std::string &s,std::vector<struct keydata> *buffer,uint64_t f_offset,int offset)
{
  std::vector<int> send_count,recv_count;

  if(invlist==nullptr) return;

  send_count.resize(numprocs); recv_count.resize(numprocs);
  std::fill(send_count.begin(),send_count.end(),0);
  std::fill(recv_count.begin(),recv_count.end(),0); 

  MPI_Request *reqs = (MPI_Request *)std::malloc(2*numprocs*sizeof(MPI_Request));

  int nreq = 0;
  
  std::vector<std::vector<double>> send_buffers;
  std::vector<std::vector<double>> recv_buffers;
  send_buffers.resize(numprocs); recv_buffers.resize(numprocs);

  int recsize = sizeof(struct event);  
  uint64_t offsets = f_offset;

  for(int i=0;i<buffer->size();i++)
  {
     KeyT key = *(KeyT*)((*buffer)[i].data+offset);
     int p = partition_no(key); 
     send_count[p]+=2;
     send_buffers[p].push_back((double)key);
     send_buffers[p].push_back(offsets);
     offsets += (uint64_t)recsize;
  }

  
  for(int i=0;i<numprocs;i++)
  {
     MPI_Isend(&send_count[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
     nreq++;
     MPI_Irecv(&recv_count[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
     nreq++;
  }

  MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

  int total_recv = 0;
  for(int i=0;i<numprocs;i++)
  {
     recv_buffers[i].resize(recv_count[i]);
     total_recv += recv_count[i];
  }

  nreq = 0;
  for(int i=0;i<numprocs;i++)
  {
    
    MPI_Isend(send_buffers[i].data(),send_count[i],MPI_DOUBLE,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
    nreq++;
    MPI_Irecv(recv_buffers[i].data(),recv_count[i],MPI_DOUBLE,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
    nreq++;
  }

  MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

  for(int i=0;i<numprocs;i++)
  {
	for(int j=0;j<recv_buffers[i].size();j+=2)
	{
	   KeyT key = (KeyT)(recv_buffers[i][j]);
	   ValueT offset = (ValueT)recv_buffers[i][j+1];
           invlist->bm->insert(key,offset);
	}
  }

  std::free(reqs);
}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::get_entries_from_tables(std::vector<struct KeyIndex<KeyT>> &KeyTimestamps,int &key_b,int &numkeys,uint64_t maxts)
{

	if(invlist==nullptr) return;

	std::vector<std::vector<KeyT>> *keys = new std::vector<std::vector<KeyT>> ();
	std::vector<std::vector<ValueT>> *offsets = new std::vector<std::vector<ValueT>> ();

	invlist->bm->fetch_clear_map(keys,offsets,maxts);

	int numentries = 0;
	for(int i=0;i<keys->size();i++)
		numentries += (*keys)[i].size();

	std::vector<int> send_counts(numprocs);
	std::vector<int> recv_counts(numprocs);
	std::fill(send_counts.begin(),send_counts.end(),0);
	std::fill(recv_counts.begin(),recv_counts.end(),0);
	send_counts[myrank] = numentries;

	MPI_Allreduce(send_counts.data(),recv_counts.data(),numprocs,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

	numkeys = 0;
	for(int i=0;i<recv_counts.size();i++)
	   numkeys += recv_counts[i];

	if(myrank==0) std::cout <<" numkeys = "<<numkeys<<std::endl;

	key_b = 0;
	for(int i=0;i<myrank;i++)
	  key_b += recv_counts[i];

	for(int i=0;i<keys->size();i++)
	{
	  for(int j=0;j<(*keys)[i].size();j++)
	  {
		struct KeyIndex<KeyT> ks;
		ks.key = (*keys)[i][j];
		ks.index = (*offsets)[i][j];
		KeyTimestamps.push_back(ks);
	  }

	}

	delete keys;
	delete offsets;

}
