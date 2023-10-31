#include "external_sort.h"
#include <string>
#include <cfloat>
#include <climits>

template<typename T>
bool compare_fields(struct event &e1, struct event &e2)
{

   T v1,v2;

   v1 = *(T*)(e1.data);
   v2 = *(T*)(e2.data);

   if(v1 < v2) return true;
   else return false; 
}

template<typename T>
std::string hdf5_sort::sort_on_secondary_key(std::string &s1_string,std::string &attr_name,int offset,uint64_t minkey,uint64_t maxkey,std::string &attr_type)
{
   std::string filename1 = "file";
   filename1 += s1_string+".h5";

   std::string filename2 = "file";
   filename2 += s1_string+"secsort";
   filename2 += ".h5";

   std::string s2_string;

    hid_t       fid;
    hid_t       acc_tpl;
    hid_t       xfer_plist;
    hid_t       file_dataspace;
    hid_t       mem_dataspace;
    hid_t       dataset1, dataset2, dataset5, dataset6, dataset7;

    xfer_plist = H5Pcreate(H5P_DATASET_XFER);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_fapl_mpio(fapl,merge_comm, MPI_INFO_NULL);
    H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

    hsize_t chunkdims[1];
    chunkdims[0] = 8192;
    hsize_t maxdims[1];
    maxdims[0] = (hsize_t)H5S_UNLIMITED;

     hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);

    int ret = H5Pset_chunk(dataset_pl,1,chunkdims);


    fid = H5Fopen(filename1.c_str(), H5F_ACC_RDONLY, fapl);

    if(fid==H5I_INVALID_HID) return s2_string;

    hid_t fid2 = H5Fcreate(filename2.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,fapl);

    H5Fclose(fid2);

    fid2 = H5Fopen(filename2.c_str(),H5F_ACC_RDWR,fapl);

    hsize_t attr_size[1];
    attr_size[0] = MAXBLOCKS*4+4;
    const char *attrname[1];
    hid_t attr_space[1];
    attr_space[0] = H5Screate_simple(1, attr_size, NULL);

    attrname[0] = "Datasizes";

    std::string data_string = "Data1";
    dataset1 = H5Dopen2(fid,data_string.c_str(), H5P_DEFAULT);

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

    int total_k = attrs[0];
    int k_size = attrs[1];
    int data_size = attrs[2];
    int numblocks = attrs[3];

    std::vector<int> blockids;
    int pos = 4;

    for(int i=0;i<numblocks;i++)
    {
        uint64_t minv = attrs[pos+i*4+0];
        uint64_t maxv = attrs[pos+i*4+1];

	blockids.push_back(i);
    }

    std::vector<struct event> *inp = new std::vector<struct event> ();

    std::vector<uint64_t> attr2;
    attr2.resize(attr_size[0]);

    hsize_t offset_w = 0;

    for(int i=0;i<blockids.size();i++)
    {

    int blockid = blockids[i];
    hsize_t offset_f = 0;
    hsize_t numrecords = attrs[pos+blockid*4+3];
    int records_per_proc = numrecords/numprocs;
    int rem = numrecords%numprocs;

    for(int j=0;j<blockid;j++)
        offset_f += attrs[pos+j*4+3];

    for(int j=0;j<myrank;j++)
    {
       int size_p = 0;
       if(j < rem) size_p = records_per_proc+1;
       else size_p = records_per_proc;
       offset_f += size_p;
    }

    hsize_t blocksize = records_per_proc;
    if(myrank < rem) blocksize++;

    inp->clear();
    inp->resize(blocksize);

    file_dataspace = H5Dget_space(dataset1);
    ret = H5Sselect_hyperslab(file_dataspace, H5S_SELECT_SET,&offset_f,NULL,&blocksize,NULL);
    mem_dataspace = H5Screate_simple(1,&blocksize, NULL);
    ret = H5Dread(dataset1,s2, mem_dataspace, file_dataspace, xfer_plist,inp->data());

    int tag = 20000;
    int minv,maxv;
    float f; double d;
    float minv_f,maxv_f;
    double minv_d,maxv_d;
    if(std::is_same<T,int>::value)
    {
        minv = INT_MAX;
        maxv = 0;
    }
    else if(std::is_same<T,float>::value)
    {
        minv_f = DBL_MAX;
        maxv_f = 0;
    }
    else if(std::is_same<T,double>::value)
    {
        minv_d = DBL_MAX;
        maxv_d = 0;
    }
    int offset_f2=0;

     if(attr_type.compare("integer")==0)
    sort_block_secondary_key<int,MPI_INT>(inp,tag,0,minv,maxv,offset_f2);
    else if(attr_type.compare("float")==0)
    sort_block_secondary_key<float,MPI_FLOAT>(inp,tag,0,minv_f,maxv_f,offset_f2);
    else if(attr_type.compare("double")==0)
    sort_block_secondary_key<double,MPI_DOUBLE>(inp,tag,0,minv_d,maxv_d,offset_f2);

    hsize_t offsetf2 = offset_f2;
    if(i==0)
    {
        offsetf2 += offset_w;
        hid_t file_dataspace2 = H5Screate_simple(1,&numrecords,maxdims);
        hsize_t block_count = inp->size();
        hid_t mem_dataspace2 = H5Screate_simple(1,&block_count, NULL);
        ret = H5Sselect_hyperslab(file_dataspace2,H5S_SELECT_SET,&offsetf2,NULL,&block_count,NULL);
        hid_t dataset2 = H5Dcreate(fid2,data_string.c_str(),s2,file_dataspace2, H5P_DEFAULT,dataset_pl,H5P_DEFAULT);
        ret = H5Dwrite(dataset2,s2, mem_dataspace2,file_dataspace2,xfer_plist,inp->data());
        attr2[0] = numrecords;
        attr2[1] = 8;
        attr2[2] = VALUESIZE;
        attr2[3] = 1;

	 int pos = 4;
        attr2[pos] = minv;
        pos++;
        attr2[pos] = maxv;
        pos++;
        attr2[pos] = 1;
        pos++;
        attr2[pos] = numrecords;

        hid_t attrid2 = H5Acreate(dataset2, attrname[0], H5T_NATIVE_UINT64, attr_space[0], H5P_DEFAULT, H5P_DEFAULT);

        ret = H5Awrite(attrid2, H5T_NATIVE_UINT64, attr2.data());

        ret = H5Aclose(attrid2);

        H5Pclose(dataset_pl);
        H5Sclose(file_dataspace2);
        H5Sclose(mem_dataspace2);
        H5Dclose(dataset2);
    }
    else
    {
        offsetf2 += offset_w;
        hid_t  dataset2 = H5Dopen(fid2,data_string.c_str(), H5P_DEFAULT);

         hsize_t dims[1];
         dims[0] = (hsize_t)(offset_w+numrecords);
         H5Dset_extent(dataset2, dims);
         hid_t file_dataspace2 = H5Dget_space(dataset2);

         hsize_t maxsize = H5S_UNLIMITED;
         hsize_t blocksize = inp->size();
         hid_t mem_dataspace2 = H5Screate_simple(1,&blocksize,&maxsize);
         hsize_t one = 1;
         ret = H5Sselect_hyperslab(file_dataspace2,H5S_SELECT_SET,&offsetf2,NULL,&blocksize,NULL);
         ret = H5Dwrite(dataset2,s2, mem_dataspace2, file_dataspace2,xfer_plist,inp->data());

         std::vector<uint64_t> attr2;
         attr2.resize(attr_size[0]);

         hid_t attrid2 = H5Aopen(dataset2,attrname[0],H5P_DEFAULT);

         ret = H5Aread(attrid2,H5T_NATIVE_UINT64,attr2.data());

         int l = attr2[3];
         attr2[3]+=1;

         int pos = 4;

	  attr2[pos+l*4+0] = minv;
         attr2[pos+l*4+1] = maxv;
         attr2[pos+l*4+2] = attr2[3];
         attr2[pos+l*4+3] = numrecords;

         ret = H5Awrite(attrid2,H5T_NATIVE_UINT64,attr2.data());

         H5Aclose(attrid2);

        H5Sclose(file_dataspace2);
        H5Sclose(mem_dataspace2);
        H5Dclose(dataset2);

    }

    offset_w += numrecords;
    H5Sclose(mem_dataspace);
    }

    delete inp;
    H5Sclose(attr_space[0]);
    H5Pclose(xfer_plist);
    H5Sclose(file_dataspace);
    H5Aclose(attr_id);
    H5Dclose(dataset1);
    H5Tclose(s2);
    H5Tclose(s1);

    H5Fclose(fid);
    H5Fclose(fid2);
    return s2_string;
}

template<typename T,int M>
void hdf5_sort::sort_block_secondary_key(std::vector<struct event> *events,int tag,int offset,T &min_value,T &max_value,int &offset_f2)
{

  MPI_Datatype value_field;
  MPI_Type_contiguous(VALUESIZE,MPI_CHAR,&value_field);
  MPI_Type_commit(&value_field);

  struct event e;
  MPI_Aint tdispl1[2];

  MPI_Get_address(&e,&tdispl1[0]);
  MPI_Get_address(&e.data,&tdispl1[1]);

  MPI_Aint base = tdispl1[0];
  MPI_Aint valuef = MPI_Aint_diff(tdispl1[1],base);

  MPI_Datatype key_value;
  int blocklens[2];
  MPI_Aint tdispl[2];
  int types[2];
  blocklens[0] = 1;
  blocklens[1] = 1;
  tdispl[0] = 0;
  tdispl[1] = valuef;
  types[0] = MPI_UINT64_T;
  types[1] = value_field;

  MPI_Type_create_struct(2,blocklens,tdispl,types,&key_value);
  MPI_Type_commit(&key_value);

  MPI_Request *reqs = (MPI_Request*)std::malloc(2*numprocs*sizeof(MPI_Request));

  int total_events = 0;

  int local_events = events->size();

   std::vector<T> mysplitters;


   if(local_events >= 2)
   {
     T r1 = random()%local_events;

     T r2 = r1;

     do
     {
        r2 = random()%local_events;
     }while(r2==r1);

     T v1,v2;

     v1 = *(T*)((*events)[r1].data+offset);
     v2 = *(T*)((*events)[r2].data+offset);

     mysplitters.push_back(v1);
     mysplitters.push_back(v2);
   }

   std::vector<int> splitter_counts(numprocs);
   std::fill(splitter_counts.begin(),splitter_counts.end(),0);
   std::vector<int> splitter_counts_l(numprocs);

   splitter_counts_l[myrank] = mysplitters.size();

   int nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
        MPI_Isend(&splitter_counts_l[myrank],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
   }

   for(int i=0;i<numprocs;i++)
   {
        MPI_Irecv(&splitter_counts[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

    int num_splitters = 0;
   for(int i=0;i<numprocs;i++) num_splitters += splitter_counts[i];


   if(num_splitters > 0)
   {
     std::vector<T> splitters;
     splitters.resize(num_splitters);

     std::vector<int> displ(numprocs);
     std::fill(displ.begin(),displ.end(),0);

     for(int i=1;i<numprocs;i++)
           displ[i] = displ[i-1]+splitter_counts[i-1];


     nreq = 0;
    for(int i=0;i<numprocs;i++)
    {
        if(splitter_counts[myrank]>0)
        {
          MPI_Isend(mysplitters.data(),splitter_counts[myrank],M,i,tag,merge_comm,&reqs[nreq]);
          nreq++;
        }
   }

    for(int i=0;i<numprocs;i++)
   {
        if(splitter_counts[i] > 0)
        {
          MPI_Irecv(&splitters[displ[i]],splitter_counts[i],M,i,tag,merge_comm,&reqs[nreq]);
          nreq++;
        }
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   std::sort(splitters.begin(),splitters.end());

   int splitters_per_proc =  num_splitters/numprocs;
   int rem = num_splitters%numprocs;
   int offset = rem*(splitters_per_proc+1);

   mysplitters.clear();

   std::vector<int> procs;

   for(int i=0;i<splitters.size();i++)
   {
        int proc=-1;
        if(i < offset)
        {
           proc = i/(splitters_per_proc+1);
        }
        else proc = rem+((i-offset)/splitters_per_proc);
        procs.push_back(proc);
   }

   std::vector<int> send_counts(numprocs);
   std::vector<int> recv_counts(numprocs);
   std::vector<int> recv_displ(numprocs);
   std::vector<int> send_displ(numprocs);
   std::fill(send_counts.begin(),send_counts.end(),0);
   std::fill(send_displ.begin(),send_displ.end(),0);
   std::fill(recv_counts.begin(),recv_counts.end(),0);
   std::fill(recv_displ.begin(),recv_displ.end(),0);

   std::vector<int> event_dest;

   std::vector<int> event_count(numprocs);
   std::fill(event_count.begin(),event_count.end(),0);

   for(int i=0;i<events->size();i++)
   {
        int dest = -1;
        T ts = *(T*)((*events)[i].data+offset);
        for(int j=0;j<splitters.size();j++)
        {
            if(ts <= splitters[j])
            {
                 dest = procs[j]; break;
            }
        }
        if(dest == -1) dest = procs[splitters.size()-1];
        send_counts[dest]++;
        event_dest.push_back(dest);
   }

   for(int i=1;i<numprocs;i++)
        send_displ[i] = send_displ[i-1]+send_counts[i-1];

   std::vector<struct event>* send_buffer = nullptr;
   std::vector<struct event>* recv_buffer = nullptr;

   std::fill(recv_displ.begin(),recv_displ.end(),0);

   nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
        MPI_Isend(&send_counts[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
        MPI_Irecv(&recv_counts[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   int total_recv_size = 0;
   for(int i=0;i<numprocs;i++)
           total_recv_size += recv_counts[i];

   send_buffer = new std::vector<struct event> (events->size());
   recv_buffer = new std::vector<struct event> (total_recv_size);

   int datasize = VALUESIZE;
   for(int i=0;i<events->size();i++)
   {
        int dest = event_dest[i];
        (*send_buffer)[send_displ[dest]] = (*events)[i];
        send_displ[dest]++;
   }

   std::fill(send_displ.begin(),send_displ.end(),0);

   for(int i=1;i<numprocs;i++)
           send_displ[i] = send_displ[i-1]+send_counts[i-1];

    for(int i=1;i<numprocs;i++)
           send_displ[i] = send_displ[i-1]+send_counts[i-1];

   for(int i=1;i<numprocs;i++)
           recv_displ[i] = recv_displ[i-1]+recv_counts[i-1];

   nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
        if(send_counts[i]>0)
        {
          MPI_Isend(&((*send_buffer)[send_displ[i]]),send_counts[i],key_value,i,tag,merge_comm,&reqs[nreq]);
          nreq++;
        }

   }

   for(int i=0;i<numprocs;i++)
   {
        if(recv_counts[i]>0)
        {
          MPI_Irecv(&((*recv_buffer)[recv_displ[i]]),recv_counts[i],key_value,i,tag,merge_comm,&reqs[nreq]);
          nreq++;
        }
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   events->clear();

   for(int i=0;i<numprocs;i++)
   {
           for(int j=0;j<recv_counts[i];j++)
           {
                struct event e = (*recv_buffer)[recv_displ[i]+j];
                events->push_back(e);
           }
   }
   std::sort(events->begin(),events->end(),compare_fields<T>);

   delete send_buffer;
   delete recv_buffer;

    offset_f2 = 0;

   int lsize = events->size();
   std::vector<int> dsizes(numprocs);
   std::fill(dsizes.begin(),dsizes.end(),0);

   nreq = 0;

   for(int i=0;i<numprocs;i++)
   {
        MPI_Isend(&lsize,1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
        MPI_Irecv(&dsizes[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   for(int i=0;i<myrank;i++)
           offset_f2 += dsizes[i];

   std::vector<T> sendk(2);


   if(std::is_same<T,int>::value)
   {
     sendk[0] = INT_MAX;
     sendk[1] = 0;
   }
   else if(std::is_same<T,float>::value)
   {
     sendk[0] = DBL_MAX;
     sendk[1] = 0;
   }
   else if(std::is_same<T,double>::value)
   {
     sendk[0] = DBL_MAX;
     sendk[1] = 0;
   }
   std::vector<T> recvkeys(2*numprocs);

   if(events->size()>0)
   {
        sendk[0] = *(T*)((*events)[0].data+offset);
        int lp = events->size();
        sendk[1] = *(T*)((*events)[lp-1].data+offset);
   }

   nreq = 0;

   for(int i=0;i<numprocs;i++)
   {
        MPI_Isend(sendk.data(),2,M,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
        MPI_Irecv(&recvkeys[2*i],2,M,i,tag,merge_comm,&reqs[nreq]);
        nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   min_value = INT_MAX;
   max_value = 0;
   for(int i=0;i<numprocs;i++)
   {
        if(recvkeys[2*i] < min_value) min_value = recvkeys[2*i];
        if(recvkeys[2*i+1]>max_value) max_value = recvkeys[2*i+1];
   }

   }
   MPI_Type_free(&key_value);
   MPI_Type_free(&value_field);
   std::free(reqs);
}

template<typename T>
void hdf5_sort::merge_tree(std::string &fname,int offset)
{
   std::string filename2 = "file";
   filename2 += fname+"secsort";
   filename2 += ".h5";

    hid_t       fid;
    hid_t       acc_tpl;
    hid_t       xfer_plist;
    hid_t       file_dataspace;
    hid_t       mem_dataspace;
    hid_t       dataset1, dataset2, dataset5, dataset6, dataset7;

    xfer_plist = H5Pcreate(H5P_DATASET_XFER);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_fapl_mpio(fapl,merge_comm, MPI_INFO_NULL);
    H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

    hid_t xfer_plist2 = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_dxpl_mpio(xfer_plist2,H5FD_MPIO_INDEPENDENT);

    hsize_t chunkdims[1];
    chunkdims[0] = 8192;
    hsize_t maxdims[1];
    maxdims[0] = (hsize_t)H5S_UNLIMITED;

    hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);

    int ret = H5Pset_chunk(dataset_pl,1,chunkdims);

    fid = H5Fopen(filename2.c_str(), H5F_ACC_RDWR, fapl);

    if(fid==H5I_INVALID_HID) return;

    hsize_t attr_size[1];
    attr_size[0] = MAXBLOCKS*4+4;
    const char *attrname[1];
    hid_t attr_space[1];
    attr_space[0] = H5Screate_simple(1, attr_size, NULL);

    attrname[0] = "Datasizes";

    std::string data_string = "Data1";
    dataset1 = H5Dopen2(fid,data_string.c_str(), H5P_DEFAULT);

    std::string data_string2 = "Data1_tmp";
    hid_t attr_id = H5Aopen(dataset1,attrname[0],H5P_DEFAULT);
    file_dataspace = H5Dget_space(dataset1);
    std::vector<uint64_t> attrs;
    attrs.resize(attr_size[0]);

    std::vector<uint64_t> attrs_new;
    attrs_new.resize(attr_size[0]);

    ret = H5Aread(attr_id,H5T_NATIVE_UINT64,attrs.data());

    hsize_t adims[1];
    adims[0] = VALUESIZE;
    hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR,1,adims);
    hid_t s2 = H5Tcreate(H5T_COMPOUND,sizeof(struct event));
    H5Tinsert(s2,"key",HOFFSET(struct event,ts),H5T_NATIVE_UINT64);
    H5Tinsert(s2,"value",HOFFSET(struct event,data),s1);

    std::fill(attrs_new.begin(),attrs_new.end(),0);

    int total_k = attrs[0];
    int k_size = attrs[1];
    int data_size = attrs[2];
    int numblocks = attrs[3];

    int nstages = std::ceil(log2(numblocks));
   
    std::vector<int> offsets;
    std::vector<int> nrecords;

    int pos = 4;

    int numrecords=0;
    int soffset = 0;
    int total_records = 0;
    for(int i=0;i<numblocks;i++)
    {
	numrecords = attrs[pos+i*4+3];
	offsets.push_back(soffset);
	nrecords.push_back(numrecords);
	soffset += numrecords;
	total_records += numrecords;
    }

    hsize_t totalrecords = total_records;

    hid_t file_dataspace2 = H5Screate_simple(1,&totalrecords,NULL);

    dataset2 = H5Dcreate(fid,data_string2.c_str(),s2,file_dataspace2, H5P_DEFAULT,dataset_pl,H5P_DEFAULT);

    hid_t attrid2 = H5Acreate(dataset2, attrname[0], H5T_NATIVE_UINT64, attr_space[0], H5P_DEFAULT, H5P_DEFAULT);

    ret = H5Awrite(attrid2, H5T_NATIVE_UINT64, attrs_new.data());


    std::vector<struct event> *block1 = new std::vector<struct event> ();
    std::vector<struct event> *block2 = new std::vector<struct event> ();
    std::vector<struct event> *sorted_block = new std::vector<struct event> ();

    int tag = 20000;

    std::vector<int> nrecords_next;

    bool last_block = false;

    hid_t file_dataspace_r = file_dataspace;
    hid_t file_dataspace_w = file_dataspace2;
    hid_t dataset_r = dataset1;
    hid_t dataset_w = dataset2;

    nstages = 0;

    while(true)
    {
        hsize_t offset_wt = 0;

       int blockid = 0;
       int numblocks_c = 0;
       for(int j=0;j<nrecords.size();j+=2)
       {
	   
	  int nrecords_t1 = nrecords[j];
	  int nrecords_t2 = (j+1 < nrecords.size()) ? nrecords[j+1] : 0;
	  last_block = false;
	  hsize_t offset_t1 = offsets[j];
	  hsize_t offset_t2 = (j+1 < nrecords.size()) ? offsets[j+1] : 0;
	  int numr_w = 0;
	  int w_offset = 0;

	  T minkey_c = INT_MAX;
	  T maxkey_c = 0;

	  int nrecords_b = 0;

	  block1->clear();

          while(nrecords_t1 > 0)
	  {	  
	    w_offset = 0;
	    hsize_t offset1 = offset_t1;
	    int numr = nrecords_t1 <= 8192 ? nrecords_t1 : 8192;
	    int numr_p = numr/numprocs;
	    int rem = numr%numprocs;
	    numr_w = numr;

	    for(int k=0;k<myrank;k++)
	    {
		int size_p = 0;
		if(k < rem) size_p = numr_p+1;
		else size_p = numr_p;
		offset1 += size_p;
		w_offset += size_p;
	    }
	    
	    hsize_t blocksize;
	    if(myrank < rem) blocksize = numr_p+1;
	    else blocksize = numr_p;
	    hsize_t maxsize = H5S_UNLIMITED;

            hid_t mem_dataspace2 = H5Screate_simple(1,&blocksize,&maxsize);

	    std::vector<struct event> *block1_t = new std::vector<struct event> ();
	    block1_t->resize(blocksize);

            ret = H5Sselect_hyperslab(file_dataspace_r, H5S_SELECT_SET,&offset1,NULL,&blocksize,NULL);
            ret = H5Dread(dataset_r,s2, mem_dataspace2, file_dataspace_r, xfer_plist,block1_t->data());

	    for(int i=0;i<block1_t->size();i++)
		    block1->push_back((*block1_t)[i]);

	    H5Sclose(mem_dataspace2);

	    nrecords_t1 -= numr;
	    offset_t1 += numr;

	    if(nrecords_t1 == 0) last_block=true;

	    if(nrecords_t2 > 0)
	    {
	      hsize_t offset2 = offset_t2;
	      numr = (nrecords_t2 <= 8192) ? nrecords_t2 : 8192; 
	      numr_p = numr/numprocs;
	      rem = numr%numprocs;

	      for(int k=0;k<myrank;k++)
	      {
		int size_p = 0;
		if(k < rem) size_p = numr_p+1;
		else size_p = numr_p;
		offset2 += size_p;
	      }

	      if(myrank < rem) blocksize = numr_p+1;
	      else blocksize = numr_p;
	      hid_t mem_dataspace1 = H5Screate_simple(1,&blocksize,&maxsize);
	      std::vector<struct event> *block2_t = new std::vector<struct event> ();
	      block2_t->resize(blocksize);
	      ret = H5Sselect_hyperslab(file_dataspace_r,H5S_SELECT_SET,&offset2,NULL,&blocksize,NULL);
     	      ret = H5Dread(dataset_r,s2,mem_dataspace1,file_dataspace_r,xfer_plist2,block2_t->data());	     

	      nrecords_t2 -= numr;
	      offset_t2 += numr;

     	      for(int k=0;k<block2_t->size();k++)
		block2->push_back((*block2_t)[k]);	     

	      delete block2_t;
	      H5Sclose(mem_dataspace1);

	   }
	   T minkey_a = INT_MAX, maxkey_a = 0;

	   sorted_block->clear();

           int nrecordsw = 0;
	  
	   if(std::is_same<T,int>::value)
	   nrecordsw = insert_block<int,MPI_INT>(block1,block2,sorted_block,offset,tag,w_offset,minkey_a,maxkey_a);

	   nrecords_b += nrecordsw;

	   numr_w = nrecordsw;

	   hsize_t numw = numr_w;
	   w_offset += offset_wt;
	   hsize_t woffset = (hsize_t)w_offset;
	   hsize_t numwb = sorted_block->size();
	   hid_t mem_dataspace_w = H5Screate_simple(1,&numwb,&maxsize);
	   ret = H5Sselect_hyperslab(file_dataspace_w,H5S_SELECT_SET,&woffset,NULL,&numwb,NULL); 
	   ret = H5Dwrite(dataset_w,s2,mem_dataspace_w,file_dataspace_w,xfer_plist,sorted_block->data());
	   H5Sclose(mem_dataspace_w);

	   sorted_block->clear();

	   attrs_new[pos+numblocks_c*4+0] = minkey_a;
	   attrs_new[pos+numblocks_c*4+1] = maxkey_a;
	   attrs_new[pos+numblocks_c*4+3] = nrecordsw;
	   attrs_new[pos+numblocks_c*4+2] = numblocks_c+1;

	   offset_wt += numr_w;

           if(last_block)
	   {
	       int offset2w = 0;
	       int total_2w = 0;
	       T maxkey_b = 0;
	       bool block_id = false;
	       if(std::is_same<T,int>::value)
	       count_offset<int,MPI_INT>(block1,block2,total_2w,offset,offset2w,tag,maxkey_b,block_id);

       	       offset2w += offset_wt;	       

	       hsize_t offsetl = (hsize_t)offset2w;
	       hsize_t total2w = (hsize_t)total_2w;
		
	       hsize_t block2size = 0;
	       if(block1->size() > 0) block2size = block1->size();
	       else block2size = block2->size();

	       hid_t mem_dataspace_2w = H5Screate_simple(1,&block2size,&maxsize);
	       ret = H5Sselect_hyperslab(file_dataspace_w,H5S_SELECT_SET,&offsetl,NULL,&block2size,NULL);
	       if(block_id)
		  ret = H5Dwrite(dataset_w,s2,mem_dataspace_2w,file_dataspace_w,xfer_plist,block1->data());
	       else 
	       ret = H5Dwrite(dataset_w,s2,mem_dataspace_2w,file_dataspace_w,xfer_plist,block2->data());
	       H5Sclose(mem_dataspace_2w);
		
	       block1->clear();
	       block2->clear();
	       offset_wt += total_2w;
	       maxkey_b = std::max(maxkey_b,(int)attrs_new[pos+numblocks_c*4+1]);
	       attrs_new[pos+numblocks_c*4+1] = maxkey_b;
	       attrs_new[pos+numblocks_c*4+3] += total_2w;
	       nrecords_b += total2w;
	       nrecords_next.push_back(nrecords_b);
	   }
	   numblocks_c++;

	  }

       }
       nrecords.clear();
       nrecords.assign(nrecords_next.begin(),nrecords_next.end());
       nrecords_next.clear();
       soffset = 0;
       offsets.clear();
       for(int k=0;k<nrecords.size();k++)
       {
	   offsets.push_back(soffset);
	   soffset += nrecords[k];
       }
       if(nrecords.size()<=1) break;
       hid_t file_dataspace_t = file_dataspace_r;
       file_dataspace_r = file_dataspace_w;
       file_dataspace_w = file_dataspace_t;
       hid_t dataset_t = dataset_r;
       dataset_r = dataset_w;
       dataset_w = dataset_t;
       nstages++;
    }

    ret = H5Awrite(attr_id,H5T_NATIVE_UINT64, attrs_new.data());
    ret = H5Awrite(attrid2,H5T_NATIVE_UINT64, attrs_new.data());
   
    hid_t del_id = (dataset_w == dataset1) ? dataset1 : dataset2;

    std::string del_str = (del_id == dataset1) ? data_string : data_string2;

    H5Ldelete(fid,del_str.c_str(),H5P_DEFAULT);


    delete block1;
    delete block2;
    delete sorted_block;

    H5Tclose(s2);
    H5Tclose(s1);

    H5Pclose(dataset_pl);
    H5Pclose(xfer_plist);
    H5Pclose(xfer_plist2);
    H5Pclose(fapl);
    H5Sclose(file_dataspace);
    H5Sclose(file_dataspace2);
    H5Sclose(attr_space[0]);
    H5Aclose(attrid2);
    H5Aclose(attr_id);
    H5Dclose(dataset2);
    H5Dclose(dataset1);
    H5Fclose(fid);
}

template<typename T,int M>
int hdf5_sort::insert_block(std::vector<struct event> *block1,std::vector<struct event> *block2,std::vector<struct event> *sorted_vec,int offset,int tag,int &offset_w,T &minkey_a,T &maxkey_a)
{
     T minv1=INT_MAX,maxv1=0;
     T minv2=INT_MAX,maxv2=0;

     MPI_Datatype value_field;
     MPI_Type_contiguous(VALUESIZE,MPI_CHAR,&value_field);
     MPI_Type_commit(&value_field);

     struct event e;
     MPI_Aint tdispl1[2];

     MPI_Get_address(&e,&tdispl1[0]);
     MPI_Get_address(&e.data,&tdispl1[1]);

     MPI_Aint base = tdispl1[0];
     MPI_Aint valuef = MPI_Aint_diff(tdispl1[1],base);

     MPI_Datatype key_value;
     int blocklens[2];
     MPI_Aint tdispl[2];
     int types[2];
     blocklens[0] = 1;
     blocklens[1] = 1;
     tdispl[0] = 0;
     tdispl[1] = valuef;
     types[0] = MPI_UINT64_T;
     types[1] = value_field;

     bool block2_empty = false;

     MPI_Type_create_struct(2,blocklens,tdispl,types,&key_value);
     MPI_Type_commit(&key_value);


     if(block1->size() > 0)
     {
	minv1 = *(T*)((*block1)[0].data+offset);
	int len1 = block1->size();
	maxv1 = *(T*)((*block1)[len1-1].data+offset);
     }

     if(block2->size() > 0)
     {
	minv2 = *(T *)((*block2)[0].data+offset);
	int len2 = block2->size();
	maxv2 = *(T *)((*block2)[len2-1].data+offset);
     }

     int nreq = 0;
     MPI_Request *reqs = (MPI_Request *)std::malloc(2*numprocs*sizeof(MPI_Request));

     std::vector<T> send_ranges(4);
     std::vector<T> recv_ranges(4*numprocs);

     send_ranges[0] = 0; send_ranges[1] = 0;
     send_ranges[2] = 0; send_ranges[3] = 0;
     std::fill(recv_ranges.begin(),recv_ranges.end(),0);


     send_ranges[0] = minv1; send_ranges[1] = maxv1;
     send_ranges[2] = minv2; send_ranges[3] = maxv2;

     for(int i=0;i<numprocs;i++)
     {
	MPI_Isend(send_ranges.data(),4,M,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recv_ranges[4*i],4,M,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     T minv1_g = INT_MAX, maxv1_g = 0;
     T minv2_g = INT_MAX, maxv2_g = 0;

     for(int i=0;i<numprocs;i++)
     {	
	   if(recv_ranges[4*i] < minv1_g) minv1_g = recv_ranges[4*i];
	   if(recv_ranges[4*i+1] > maxv1_g) maxv1_g = recv_ranges[4*i+1];
	   if(recv_ranges[4*i+2] < minv2_g) minv2_g = recv_ranges[4*i+2];
	   if(recv_ranges[4*i+3] > maxv2_g) maxv2_g = recv_ranges[4*i+3];
     }

     if(minv2_g==INT_MAX && maxv2_g==0) block2_empty = true;

     std::vector<int> send_count,recv_count;
     send_count.resize(numprocs);
     recv_count.resize(numprocs);

     std::fill(send_count.begin(),send_count.end(),0);
     std::fill(recv_count.begin(),recv_count.end(),0);

     std::vector<int> dest;

     for(int i=0;i<block2->size();i++)
     {
	T key = *(T*)((*block2)[i].data+offset);
	int dest_proc = -1;
	for(int j=0;j<numprocs;j++)
	{
	    if(key <= recv_ranges[4*j] || (key >= recv_ranges[4*j] && key <= recv_ranges[4*j+1]))
	    {
		send_count[j]++; dest_proc = j; break;
	    }	    
	}
	dest.push_back(dest_proc);
     }

     nreq = 0;

     for(int i=0;i<numprocs;i++)
     {
	MPI_Isend(&send_count[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recv_count[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     std::vector<std::vector<struct event>*> send_buffers;
     std::vector<std::vector<struct event>*> recv_buffers;

     send_buffers.resize(numprocs);
     recv_buffers.resize(numprocs);

     for(int i=0;i<numprocs;i++)
     {
	send_buffers[i] = new std::vector<struct event> ();
	recv_buffers[i] = new std::vector<struct event> ();
     }

     std::vector<struct event> *block2_g = new std::vector<struct event> ();

     for(int i=0;i<block2->size();i++)
     {
       if(dest[i] != -1)
       {	    
	  send_buffers[dest[i]]->push_back((*block2)[i]);
       }
       else block2_g->push_back((*block2)[i]);
     }

     block2->clear();

     int total_recv = 0;
     for(int i=0;i<numprocs;i++)
     {	
	total_recv += recv_count[i];
	if(recv_count[i]>0) recv_buffers[i]->resize(recv_count[i]);
     }

     nreq = 0;
     for(int i=0;i<numprocs;i++)
     {
	if(send_count[i]>0)
	{
	  MPI_Isend(send_buffers[i]->data(),send_count[i],key_value,i,tag,merge_comm,&reqs[nreq]);
	  nreq++;
	}
	if(recv_count[i]>0)
	{
	  MPI_Irecv(recv_buffers[i]->data(),recv_count[i],key_value,i,tag,merge_comm,&reqs[nreq]);
	  nreq++;
	}
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     std::vector<struct event> *block2_range = new std::vector<struct event> ();

     for(int i=0;i<numprocs;i++)
     {
	for(int j=0;j<recv_buffers[i]->size();j++)
	   block2_range->push_back((*recv_buffers[i])[j]);
     }

     block2->assign(block2_range->begin(),block2_range->end());
     block2_range->clear();

     T min_g = minv1_g;
     T max_g = maxv1_g;

     if(!block2_empty) 
     {
	min_g = std::min(min_g,minv2_g);
	max_g = std::min(max_g,maxv2_g);
     }

     std::vector<struct event> *block1_f = new std::vector<struct event> ();

     for(int i=0;i<block1->size();i++)
     {
	T key = *(T *)((*block1)[i].data+offset);
	if(key > max_g) block1_f->push_back((*block1)[i]);
     }

     int i=0,j=0;
     while(i < block2->size())
     {
	if(j == block1->size()) break;
	T key1 = *(T*)((*block1)[j].data+offset);
        T key2 = *(T*)((*block2)[i].data+offset);

	if(key2 <= key1)
	{
	  while(i < block2->size())
	  {
	    T key = *(T*)((*block2)[i].data+offset);
	    if(key <= key1) 
	    {
	      if(key <= max_g) 
	      {
		 sorted_vec->push_back((*block2)[i]);
	         i++;
	      }
	      else break;
	    }
	    else break;
	  }
	}
	else
	{
	   if(key1 <= key2)
	   {
		while(j < block1->size())
		{	
		   T key = *(T *)((*block1)[j].data+offset);
		   if(key <= key2) 
		   {
		     if(key <= max_g) 
		     {
			sorted_vec->push_back((*block1)[j]);
		        j++;
		     }
		     else break;
		   }
		   else break;
		}	
	   }

	}

     }
  
     while(j < block1->size())
     {
	T key = *(T*)((*block1)[j].data+offset);
	if(key <= max_g) 
	{
	  sorted_vec->push_back((*block1)[j]);
	  j++;
	}
	else break;
     }

     while(i < block2->size())
     {
	T key = *(T*)((*block2)[i].data+offset);
	if(key <= max_g) 
	{
	   sorted_vec->push_back((*block2)[i]);
	   i++;
	}
	else break;
     }

     block1->clear();
     block2->clear();

     int lo_size = block1_f->size();
     std::vector<int> recvlo_sizes(numprocs);
     std::fill(recvlo_sizes.begin(),recvlo_sizes.end(),0);

     nreq = 0;
	
     MPI_Isend(&lo_size,1,MPI_INT,0,tag,MPI_COMM_WORLD,&reqs[nreq]);
     nreq++;

     if(myrank==0)
     {
	for(int i=0;i<numprocs;i++)
	{
	   MPI_Irecv(&recvlo_sizes[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	}
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     if(myrank==0)
     for(int i=0;i<numprocs;i++) 
     {
	 recv_buffers[i]->clear();
	 if(recvlo_sizes[i]>0)
	 recv_buffers[i]->resize(recvlo_sizes[i]);
     }
     
     nreq = 0;
    
     if(lo_size>0)
     {
       MPI_Isend(block1_f->data(),lo_size,key_value,0,tag,MPI_COMM_WORLD,&reqs[nreq]);
       nreq++;
     }

     if(myrank==0)
     {
	for(int i=0;i<numprocs;i++)
	{
           if(recvlo_sizes[i]>0)
	   {
	     MPI_Irecv(recv_buffers[i]->data(),recvlo_sizes[i],key_value,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	     nreq++;
	   }
	}
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     block1_f->clear();

     if(myrank==0)
     {	
	for(int i=0;i<numprocs;i++)
	{
	   for(int j=0;j<recv_buffers[i]->size();j++)
	   {
		block1_f->push_back((*recv_buffers[i])[j]);
	   }
	}
	block1->assign(block1_f->begin(),block1_f->end());
     }

     lo_size = block2_g->size();

     std::fill(recvlo_sizes.begin(),recvlo_sizes.end(),0);

     nreq = 0;
     MPI_Isend(&lo_size,1,MPI_INT,0,tag,MPI_COMM_WORLD,&reqs[nreq]);
     nreq++;

     if(myrank==0)
     {
	for(int i=0;i<numprocs;i++)
	{
	  MPI_Irecv(&recvlo_sizes[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	  nreq++;
	}
     }
     
     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     if(myrank==0)
     for(int i=0;i<numprocs;i++)
     {
	recv_buffers[i]->clear();
	if(recvlo_sizes[i]>0) recv_buffers[i]->resize(recvlo_sizes[i]);
     }

     nreq = 0;
     if(lo_size>0)
     {
         MPI_Isend(block2_g->data(),lo_size,key_value,0,tag,MPI_COMM_WORLD,&reqs[nreq]);
	 nreq++;
     }

     if(myrank==0)
     {
	for(int i=0;i<numprocs;i++)
	{
	   if(recvlo_sizes[i]>0)
	   {
		MPI_Irecv(recv_buffers[i]->data(),recvlo_sizes[i],key_value,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
		nreq++;
	   }
	}
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     block2_g->clear();

     if(myrank==0)
     {
	for(int i=0;i<numprocs;i++)
	{
	   for(int j=0;j<recv_buffers[i]->size();j++)
	   {
		block2_g->push_back((*recv_buffers[i])[j]);
	   }
	}
	block2->assign(block2_g->begin(),block2_g->end());
     }

     int ssize = sorted_vec->size();

     std::vector<int> rsize(numprocs);
     std::fill(rsize.begin(),rsize.end(),0);

     nreq = 0;

     for(int i=0;i<numprocs;i++)
     {
	MPI_Isend(&ssize,1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&rsize[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     int total_size = 0;

     for(int i=0;i<rsize.size();i++) total_size += rsize[i];

     offset_w = 0;

     for(int i=0;i<myrank;i++)
	     offset_w += rsize[i];

     send_ranges[0] = INT_MAX;
     send_ranges[1] = 0;

     std::fill(recv_ranges.begin(),recv_ranges.end(),0);

     if(sorted_vec->size()>0)
     {
	send_ranges[0] = *(T*)((*sorted_vec)[0].data+offset);
	int len = sorted_vec->size();
	send_ranges[1] = *(T*)((*sorted_vec)[len-1].data+offset);
     }

     nreq = 0;
     for(int i=0;i<numprocs;i++)
     {
	MPI_Isend(send_ranges.data(),2,M,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recv_ranges[2*i],2,M,i,tag,merge_comm,&reqs[nreq]);
	nreq++;
     }

     MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

     minkey_a = INT_MAX; maxkey_a = 0;
     
     for(int i=0;i<numprocs;i++)
     {
           if(recv_ranges[2*i] < minkey_a) minkey_a = recv_ranges[2*i];
   	   if(recv_ranges[2*i+1] > maxkey_a) maxkey_a = recv_ranges[2*i+1];	   
     }

     //if(myrank==0) std::cout <<" minkey_a = "<<minkey_a<<" maxkey_a = "<<maxkey_a<<std::endl;

     for(int i=0;i<numprocs;i++)
     {
	delete send_buffers[i];
	delete recv_buffers[i];
     }

     delete block2_range;
     delete block2_g;
     delete block1_f;
     MPI_Type_free(&key_value);
     MPI_Type_free(&value_field);
     std::free(reqs);
     return total_size;
}

template<typename T,int M>
void hdf5_sort::count_offset(std::vector<struct event> *block1,std::vector<struct event> *block2,int &total_records,int offset_a,int &offset,int tag,T &maxkey,bool &block_id)
{

	int local_size = 0;

	if(block1->size() > 0) local_size = block1->size();
	else local_size = block2->size();

	MPI_Request *reqs = (MPI_Request *)std::malloc(2*numprocs*sizeof(MPI_Request));

	int nreq = 0;
	std::vector<int> rsizes(numprocs);
	std::fill(rsizes.begin(),rsizes.end(),0);

	for(int i=0;i<numprocs;i++)
	{
	   MPI_Isend(&local_size,1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
	   nreq++;
	   MPI_Irecv(&rsizes[i],1,MPI_INT,i,tag,merge_comm,&reqs[nreq]);
	   nreq++;
	}

	MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

	total_records = 0;
	for(int i=0;i<numprocs;i++)
	   total_records += rsizes[i];

	offset = 0;
	for(int i=0;i<myrank;i++)
	   offset += rsizes[i];

	maxkey = 0;
	T send_key = 0;
	std::vector<T> recv_keys(numprocs);

	int send_block_id = 0;

	if(block1->size()>0) send_block_id = 1;
	else if(block2->size()>0) send_block_id = 2;

	std::vector<int> recv_block_id(numprocs);
	std::fill(recv_block_id.begin(),recv_block_id.end(),0);

	nreq = 0;
	for(int i=0;i<numprocs;i++)
	{
	   MPI_Isend(&send_block_id,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	   MPI_Irecv(&recv_block_id[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	}

	MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

	int rblock=0;

	for(int i=0;i<numprocs;i++)
	{
	   if(rblock < recv_block_id[i]) rblock = recv_block_id[i];
	}

	if(rblock==1) block_id = true;
	else block_id = false;


	if(block1->size()>0)
	{
	   int len = block1->size();
	   send_key = *(T *)((*block1)[len-1].data);

	}
	else if(block2->size()>0)
	{
	    int len = block2->size();
	    send_key = *(T *)((*block2)[len-1].data);
	}

	nreq = 0;
	for(int i=0;i<numprocs;i++)
	{
	   MPI_Isend(&send_key,1,M,i,tag,merge_comm,&reqs[nreq]);
	   nreq++;
	   MPI_Irecv(&recv_keys[i],1,M,i,tag,merge_comm,&reqs[nreq]);
	   nreq++;
	}

	MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

	for(int i=0;i<numprocs;i++)
		if(recv_keys[i] > maxkey) maxkey = recv_keys[i];

	std::free(reqs);
}

/*
template<typename T>
std::string hdf5_sort::merge_datasets(std::string &s1,std::string &s2)
{
    std::string s3 = "merged_file";





    return s3;
}

std::string hdf5_sort::merge_stream_with_dataset(std::string &s,std::vector<struct event>* events)
{
   std::string s2 = "merged_file";



   return s2;

}
       
std::string hdf5_sort::merge_multiple_dataset(std::vector<std::string>& snames)
{
    std::string s2 = "merged_file";






   return s2;
}*/

