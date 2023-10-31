#include "inverted_list.h"


template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
int hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::partition_no(KeyT &k)
{

      uint64_t hashval = hashfcn()(k);
      int np = nearest_power_two(numprocs);   
      int nbits = log2(np);
      uint64_t key = hashval;
      key = key >> (64-nbits);   
      int id = (int)key;
      return id;
}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::create_invlist(std::string &s,int maxsize)
{

	struct head_node<KeyT,ValueT,hashfcn,equalfcn> *h  = new struct head_node<KeyT,ValueT,hashfcn,equalfcn>();

	h->maxsize = maxsize;

	int maxkey_int = INT_MAX;
	float maxkey_float = DBL_MAX;
	double maxkey_double = DBL_MAX;

	KeyT maxkey;
        if(std::is_same<KeyT,int>::value) maxkey = (KeyT)maxkey_int;
	else if(std::is_same<KeyT,float>::value) maxkey = (KeyT)maxkey_float;
	else if(std::is_same<KeyT,double>::value) maxkey = (KeyT)maxkey_double;	


	h->table = new struct invnode<KeyT,ValueT,hashfcn,equalfcn> ();
	h->table->ml = new memory_pool<KeyT,ValueT,hashfcn,equalfcn> (100);
	h->table->bm = new BlockMap<KeyT,ValueT,hashfcn,equalfcn> (maxsize,h->table->ml,maxkey); 

	std::pair<std::string,struct head_node<KeyT,ValueT,hashfcn,equalfcn>*> p;
	p.first.assign(s);
	p.second = h;
	invlists.insert(p);
}


template<typename KeyT, typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::fill_invlist_from_file(std::string &s,int offset)
{
   std::string filename = "file";
   filename += s+".h5";

   hid_t xfer_plist = H5Pcreate(H5P_DATASET_XFER);
   hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
   H5Pset_fapl_mpio(fapl,MPI_COMM_WORLD, MPI_INFO_NULL);
   H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

   auto r = invlists.find(s);

   struct head_node<KeyT,ValueT,hashfcn,equalfcn> *h  = r->second;

   hsize_t chunkdims[1];
   chunkdims[0] = 8192;
   hsize_t maxdims[1];
   maxdims[0] = (hsize_t)H5S_UNLIMITED;

   hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);

   int ret = H5Pset_chunk(dataset_pl,1,chunkdims);

   hid_t fid = H5Fopen(filename.c_str(),H5F_ACC_RDWR,fapl);     

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
    
    int key_pre=0;
    int totalkeys=0;

    std::vector<std::vector<KeyT>> *keys = new std::vector<std::vector<KeyT>> ();
    std::vector<std::vector<ValueT>> *offsets = new std::vector<std::vector<ValueT>> ();

    get_entries_from_tables(s,keys,offsets,key_pre,totalkeys);

    std::vector<struct KeyIndex<KeyT>> *buf = nullptr;

    buf = new std::vector<struct KeyIndex<KeyT>> ();
    
    for(int i=0;i<keys->size();i++)
    {
	for(int j=0;j<(*keys)[i].size();j++)
	{	
		struct KeyIndex<KeyT> nk;
		nk.key = (*keys)[i][j];
		nk.index = (*offsets)[i][j];
		buf->push_back(nk);
	 }
    }

    delete keys;
    delete offsets;

    hsize_t blockcount = 0;
    if(buf != nullptr) blockcount = buf->size();

    hsize_t offset_w = (hsize_t)key_pre;
    std::string kv_string = "key_index";
    hsize_t totalkeys_t = (hsize_t)totalkeys;
    hid_t file_dataspace2 = H5Screate_simple(1,&totalkeys_t,maxdims);
    hid_t dataset2 = H5Dcreate(fid,kv_string.c_str(),kv1,file_dataspace2, H5P_DEFAULT,dataset_pl,H5P_DEFAULT);
    hid_t mem_dataspace2 = H5Screate_simple(1,&blockcount, NULL);
    ret = H5Sselect_hyperslab(file_dataspace2,H5S_SELECT_SET,&offset_w,NULL,&blockcount,NULL);
    ret = H5Dwrite(dataset2,kv1, mem_dataspace2,file_dataspace2,xfer_plist,buf->data());
    H5Sclose(file_dataspace2);
    H5Sclose(mem_dataspace2);

    H5Dclose(dataset2);
    if(buf != nullptr) delete buf;
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
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::flush_table_file(std::string &s,int offset)
{
 std::string filename = "file";
 filename += s+".h5";

 hid_t xfer_plist = H5Pcreate(H5P_DATASET_XFER);
 hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
 H5Pset_fapl_mpio(fapl,MPI_COMM_WORLD, MPI_INFO_NULL);
 H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

 hid_t xfer_plist2 = H5Pcreate(H5P_DATASET_XFER);
 H5Pset_dxpl_mpio(xfer_plist2,H5FD_MPIO_INDEPENDENT);

 auto r = invlists.find(s);

 struct head_node<KeyT,ValueT,hashfcn,equalfcn> *h  = r->second;

 hsize_t chunkdims[1];
 chunkdims[0] = 8192;
 hsize_t maxdims[1];
 maxdims[0] = (hsize_t)H5S_UNLIMITED;

 hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);

 int ret = H5Pset_chunk(dataset_pl,1,chunkdims);

 hid_t fid = H5Fopen(filename.c_str(),H5F_ACC_RDWR,fapl);

 hsize_t adims[1];
 adims[0] = VALUESIZE;
 hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR,1,adims);
 hid_t s2 = H5Tcreate(H5T_COMPOUND,sizeof(struct event));
 H5Tinsert(s2,"key",HOFFSET(struct event,ts),H5T_NATIVE_UINT64);
 H5Tinsert(s2,"value",HOFFSET(struct event,data),s1);

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


 std::vector<std::vector<KeyT>> *keys = new std::vector<std::vector<KeyT>> ();
 std::vector<std::vector<ValueT>> *timestamps = new std::vector<std::vector<ValueT>> ();

 int key_pre = 0;
 int total_keys = 0;

 get_entries_from_tables(s,keys,timestamps,key_pre,total_keys); 

 std::vector<struct KeyIndex<int>>  KeyTimestamps;

 for(int i=0;i<keys->size();i++)
 {
    for(int j=0;j<(*keys)[i].size();j++)
    {
	struct KeyIndex<int> k;
	k.key = (*keys)[i][j];
	k.index = (*timestamps)[i][j];
	KeyTimestamps.push_back(k);	
   }
 } 

 delete keys;
 delete timestamps;

 std::sort(KeyTimestamps.begin(),KeyTimestamps.end(),compareIndex<int>);

 int numblocks = attrs[3];

 int block_id = 0;
 int pos = 4;

 std::vector<int> block_ids;
 int i=0;

 int min_block = INT_MAX;
 int max_block = INT_MIN;

 while(i < KeyTimestamps.size())
 {
   if(block_id >= numblocks) break;
   uint64_t ts = KeyTimestamps[i].index; 
   uint64_t minkey = attrs[pos+block_id*4+0];
   uint64_t maxkey = attrs[pos+block_id*4+1];
   if(ts >= minkey && ts <= maxkey) 
   {
	block_ids.push_back(block_id); i++;
	if(min_block > block_id) min_block = block_id;
	if(max_block < block_id) max_block = block_id;
   }
   else if(ts < minkey) 
   {
	   block_ids.push_back(-1);
	   i++;
   }
   else if(ts > maxkey) block_id++;
 }

 while(i < KeyTimestamps.size())
 {
    block_ids.push_back(-1);
    i++;
 }

 hid_t file_dataspace = H5Dget_space(dataset1);

 std::vector<struct event> *buffer = new std::vector<struct event> ();


 block_id = -1;

 i = 0;

 while(i < block_ids.size())
 {
    block_id = block_ids[i];

    while(block_id == -1)
    {
	i++;
	if(i == block_ids.size()) break;
    }
    if(i==block_ids.size()) break;

    block_id = block_ids[i];

    int nrecords = attrs[pos+block_id*4+3];

    hsize_t pre = 0;
    for(int k=0;k<block_id;k++)
    {
           pre += attrs[pos+k*4+3];
    }
    
    hsize_t blocksize = nrecords;
        
    buffer->clear();
    buffer->resize(blocksize);

    ret = H5Sselect_hyperslab(file_dataspace, H5S_SELECT_SET,&pre,NULL,&blocksize,NULL);
    hid_t mem_dataspace = H5Screate_simple(1,&blocksize, NULL);
    ret = H5Dread(dataset1,s2, mem_dataspace, file_dataspace, xfer_plist2,buffer->data());
    H5Sclose(mem_dataspace);

    int k = 0;

    while(block_ids[i]==block_id)
    {
	if(k == buffer->size()||i==KeyTimestamps.size()) break;

	if(KeyTimestamps[i].index==(*buffer)[k].ts)
	{
	   KeyTimestamps[i].index = pre+k;
	   k++;i++;
	}
	else if(KeyTimestamps[i].index < (*buffer)[k].ts)
	{
		i++;
	}
	else if(KeyTimestamps[i].index > (*buffer)[k].ts)
	{
		k++;
	}

    }


 } 

 int j=0;
 int k=0;
 for(i=0;i<KeyTimestamps.size();)
 {
	for(j=i;j<KeyTimestamps.size();)
	{
	    if(block_ids[j]==-1) j++;
	}

	int gap = j-i;

	for(k=j;k<KeyTimestamps.size();k++)
	{
	   if(block_ids[k]==-1) 
	   {
		   break;
	   }
	   KeyTimestamps[k-gap] = KeyTimestamps[k];
	}
	i = k-gap;

 }



 delete buffer;

 H5Dclose(dataset1);
 H5Sclose(file_dataspace);
 H5Aclose(attr_id);
 H5Tclose(s2);
 H5Tclose(s1);
 H5Pclose(dataset_pl);
 H5Fclose(fid);



}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::add_entries_to_tables(std::string &s,std::vector<struct event> *buffer,uint64_t f_offset,int offset)
{
  std::vector<int> send_count,recv_count;

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

  auto r = invlists.find(s);
  struct head_node<KeyT,ValueT,hashfcn,equalfcn> *h = r->second; 
  struct invnode<KeyT,ValueT,hashfcn,equalfcn> *table = h->table;

  for(int i=0;i<numprocs;i++)
  {
	for(int j=0;j<recv_buffers[i].size();j+=2)
	{
	   KeyT key = (KeyT)(recv_buffers[i][j]);
	   ValueT offset = (ValueT)recv_buffers[i][j+1];
           table->bm->insert(key,offset);
	}
  }

  std::free(reqs);
}

template<typename KeyT,typename ValueT,typename hashfcn,typename equalfcn>
void hdf5_invlist<KeyT,ValueT,hashfcn,equalfcn>::get_entries_from_tables(std::string &s,std::vector<std::vector<KeyT>> *keys,std::vector<std::vector<ValueT>> *offsets,int &key_b,int &numkeys)
{

	auto r = invlists.find(s);

	struct head_node<KeyT,ValueT,hashfcn,equalfcn> *h = r->second;

	struct invnode<KeyT,ValueT,hashfcn,equalfcn> *table = h->table;

	table->bm->get_map_keyvalue(keys,offsets);

	table->bm->clear_map();

	MPI_Request *reqs = (MPI_Request *)std::malloc(2*numprocs*sizeof(MPI_Request));
	int nreq = 0;

	int numentries = 0;
	for(int i=0;i<keys->size();i++)
		numentries += (*keys)[i].size();

	std::vector<int> recv_counts(numprocs);
	int send_count = 0;
	send_count = numentries;

	for(int i=0;i<numprocs;i++)
	{
	   MPI_Isend(&send_count,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	   MPI_Irecv(&recv_counts[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	}

	MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

	numkeys = 0;
	for(int i=0;i<recv_counts.size();i++)
	   numkeys += recv_counts[i];

	key_b = 0;
	for(int i=0;i<myrank;i++)
	  key_b += recv_counts[i];


	std::free(reqs);
}
