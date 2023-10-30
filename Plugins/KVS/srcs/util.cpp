#include "util_t.h"
#include <fstream>

#define STRUCTEV(n)\
struct keydata\
{\
   uint64_t ts; \
   char data[n];\
};

int nearest_power_two(int n)
{
   int c = 1;
   while(c < n) c = 2*c;
   return c;
}

void create_integertestinput(std::string &name,int numprocs,int myrank,int offset,std::vector<int> &keys,std::vector<uint64_t> &timestamps)
{
   std::string filename = "file";
   filename += name+".h5";
   hid_t xfer_plist = H5Pcreate(H5P_DATASET_XFER);
   hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
   H5Pset_fapl_mpio(fapl,MPI_COMM_WORLD, MPI_INFO_NULL);
   H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

   hid_t fid = H5Fopen(filename.c_str(),H5F_ACC_RDONLY,fapl);

   hsize_t attr_size[1];
   attr_size[0] = MAXBLOCKS*4+4;
   const char *attrname[1];
   hid_t attr_space[1];
   attr_space[0] = H5Screate_simple(1, attr_size, NULL);

   attrname[0] = "Datasizes";

  
   std::string data_string = "Data1";
   hid_t dataset1 = H5Dopen2(fid,data_string.c_str(), H5P_DEFAULT);

   hid_t file_dataspace = H5Dget_space(dataset1);
   hid_t attr_id = H5Aopen(dataset1,attrname[0],H5P_DEFAULT);
   std::vector<uint64_t> attrs;
   attrs.resize(attr_size[0]);

   int ret = H5Aread(attr_id,H5T_NATIVE_UINT64,attrs.data());

   hsize_t adims[1];
   adims[0] = attrs[2];
   hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR,1,adims);
   hid_t s2 = H5Tcreate(H5T_COMPOUND,sizeof(struct event));
   if(s1< 0 || s2 < 0) std::cout <<" data types "<<std::endl;
   H5Tinsert(s2,"key",HOFFSET(struct event,ts),H5T_NATIVE_UINT64);
   H5Tinsert(s2,"value",HOFFSET(struct event,data),s1);

 

   int pos = 4;
   int numblocks = attrs[3];

   int offset_r = 0;
   std::vector<struct event> *buffer = new std::vector<struct event> ();

   for(int i=0;i<numblocks;i++)
   {
	int nrecords = attrs[pos+i*4+3];

        int records_per_proc = nrecords/numprocs;
        int rem = nrecords%numprocs;

        hsize_t pre = (hsize_t)offset_r;
        for(int j=0;j<myrank;j++)
        {
           int size_p=0;
           if(j < rem) size_p = records_per_proc+1;
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
	
	offset_r += nrecords;

	for(int j=0;j<buffer->size();j++)
	{
	   timestamps.push_back((*buffer)[j].ts);
	   int key = *(int*)((*buffer)[j].data+offset);
	   keys.push_back(key);
	}
   }

   delete buffer;
   H5Aclose(attr_id);
   H5Sclose(attr_space[0]);
   H5Dclose(dataset1);
   H5Tclose(s1);
   H5Tclose(s2);
   H5Sclose(file_dataspace);
   H5Fclose(fid);
   H5Pclose(fapl);
   H5Pclose(xfer_plist);
  

}

void create_ycsb_input(std::string &filename,int numprocs,int myrank,std::vector<uint64_t> &keys,std::vector<std::string> &values,std::vector<int> &op)
{

   int columnsize = 100;

   std::ifstream ist(filename.c_str(),std::ios_base::in);
   std::vector<std::string> lines;

   std::vector<int> ops_t;

   if(ist.is_open())
   {
     std::string line;
     while(getline(ist,line))
     {
        std::stringstream ss(line);
        std::string string1;
        ss >> string1;
        if(string1.compare("INSERT")==0||string1.compare("READ")==0||string1.compare("UPDATE")==0)
	{
          lines.push_back(line);
	  if(string1.compare("INSERT")==0) ops_t.push_back(0);
	  else if(string1.compare("READ")==0) ops_t.push_back(1);
	  else ops_t.push_back(2);
	}

     }

     std::string str1 = "usertable";
     std::string str2 = "user";
     std::vector<std::string> fields;
     fields.push_back("field0=");
     fields.push_back("field1=");
     fields.push_back("field2=");
     fields.push_back("field3=");
     fields.push_back("field4=");
     fields.push_back("field5=");
     fields.push_back("field6=");
     fields.push_back("field7=");
     fields.push_back("field8=");
     fields.push_back("field9=");
     std::string b1 = "[";
     std::string b2 = "]";

     int totalrecords = lines.size();
     int records_per_proc = totalrecords/numprocs;
     int rem = totalrecords%numprocs;

     int numrecords = records_per_proc;
     if(myrank < rem) numrecords++;

     int offset = 0;
     for(int i=0;i<myrank;i++)
     {
	if(i < rem) offset += records_per_proc+1;
	else offset += records_per_proc;

     }

     for(int i=offset;i<offset+numrecords;i++)
     {
       int pos1 = lines[i].find(str1.c_str())+str1.length();
       std::string substr1 = lines[i].substr(pos1);
       std::stringstream ss(substr1);
       std::string tss;
       ss >> tss;
       int pos2 = tss.find(str2.c_str())+str2.length();
       std::string substr2 = tss.substr(pos2);
       uint64_t key = std::stoul(substr2,nullptr);
       std::vector<std::string> fieldstrings;
       fieldstrings.resize(fields.size());
       std::string data;
       if(ops_t[i]==0 || ops_t[i]==2)
       for(int j=0;j<fields.size();j++)
       {
         pos1 = lines[i].find(fields[j].c_str());
         if(pos1 != std::string::npos)
	 {
	 if(ops_t[i]==2) data = fields[j];
         pos1+=fields[j].length();
         substr1 = lines[i].substr(pos1);
         pos2 = substr1.find("field");
         fieldstrings[j].resize(columnsize);
         if(pos2 != std::string::npos)
         {
           substr1 = lines[i].substr(pos1,pos2-1);
           for(int k=0;k<substr1.length();k++)
           fieldstrings[j][k] = substr1[k];
         }
         else
         {
            pos2 = substr1.find("]");
            if(pos2 != std::string::npos)
            {
              substr1 = lines[i].substr(pos1,pos2-1);
              for(int k=0;k<substr1.length();k++)
              fieldstrings[j][k] = substr1[k];
            }
         }
         data += fieldstrings[j];
	 }
       }
       keys.push_back(key);
       values.push_back(data);
       op.push_back(ops_t[i]);
     }

     ist.close();
   }

}

void create_timeseries_testinput(std::string &filename,int numprocs,int myrank,std::vector<uint64_t> &keys,std::vector<float> &values,std::vector<int>&op)
{
   if(myrank==0)
   {
    std::ifstream ist(filename.c_str(),std::ios_base::in);

    std::vector<std::string> lines;

    if(ist.is_open())
    {
     std::string line;
     while(getline(ist,line))
     {
        std::stringstream ss(line);
        std::string string1;
        ss >> string1;
        if(string1.compare("INSERT")==0||string1.compare("READ")==0)
	{
          lines.push_back(line);
	  if(string1.compare("INSERT")==0) op.push_back(0);
	  else op.push_back(1);
	}
     }

     std::cout <<" numlines = "<<lines.size()<<std::endl;
     std::string str1 = "YCSBTS=";
     std::string str2 = "YCSBV=";
     for(int i=0;i<lines.size();i++)
     {
       if(op[i]==0)
       {
         int pos1 = lines[i].find(str1.c_str())+str1.length();
         std::string substr1 = lines[i].substr(pos1);
         std::stringstream ss(substr1);
         std::string tss;
         ss >> tss;
         uint64_t tsu = std::stoul(tss,nullptr,0);

         int pos2 = substr1.find(str2.c_str())+str2.length();
         std::string substr2 = substr1.substr(pos2);

         std::stringstream ess(substr2);
         std::string valuestr;
         ess >> valuestr;
         float value = std::stof(valuestr,nullptr);

         keys.push_back(tsu);
         values.push_back(value);
       }
       else
       {
	  uint64_t tsu = UINT64_MAX;
	  float value = 0;
	  keys.push_back(tsu);
	  values.push_back(value);
	
       }
     }

     ist.close();
   }

   }
   
   int numrecords = keys.size();
   int totalrecords = 0;
   MPI_Allreduce(&numrecords,&totalrecords,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);

   int records_per_proc = totalrecords/numprocs;
   int rem = totalrecords%numprocs;

   if(myrank < rem) numrecords = records_per_proc+1;
   else numrecords = records_per_proc;

   if(myrank != 0) 
   {
      values.resize(numrecords);
      keys.resize(numrecords);
      op.resize(numrecords);
   }

   MPI_Request *reqs = (MPI_Request*)std::malloc(2*numprocs*sizeof(MPI_Request));
   int nreq = 0;

   int offset = 0;
   if(myrank==0)
   for(int i=0;i<numprocs;i++)
   {
	int nrecords = 0;
	if(i < rem) nrecords = records_per_proc+1;
	else nrecords = records_per_proc;
	if(i != 0)
	{
	  MPI_Isend(&keys[offset],nrecords,MPI_UINT64_T,i,123,MPI_COMM_WORLD,&reqs[nreq]);
	  nreq++;
        }
	offset += nrecords;
   }
   else
   {
	MPI_Irecv(keys.data(),numrecords,MPI_UINT64_T,0,123,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   if(myrank==0)
   {
	std::vector<uint64_t> temp;
	temp.assign(keys.begin(),keys.begin()+numrecords);
	keys.assign(temp.begin(),temp.end());

   }

   nreq = 0;
   offset = 0;
   if(myrank==0)
   for(int i=0;i<numprocs;i++)
   {
      int nrecords = 0;
      if(i < rem) nrecords = records_per_proc+1;
      else nrecords = records_per_proc;

      if(i != 0)
      {
	MPI_Isend(&values[offset],nrecords,MPI_FLOAT,i,123,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
      }
      offset += nrecords;
   }
   else
   {
	MPI_Irecv(values.data(),numrecords,MPI_FLOAT,0,123,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   if(myrank==0)
   {
	std::vector<float> temp;
	temp.assign(values.begin(),values.begin()+numrecords);
	values.assign(temp.begin(),temp.end());
   }

   offset = 0;
   nreq = 0;
   if(myrank==0)
   for(int i=0;i<numprocs;i++)
   {
	int nrecords = 0;
	if(i < rem) nrecords = records_per_proc+1;
	else nrecords = records_per_proc;

	if(i != 0)
	{	
	   MPI_Isend(&op[offset],nrecords,MPI_INT,i,123,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	}
	offset += nrecords;
   }
   else
   {
	MPI_Irecv(op.data(),numrecords,MPI_INT,0,123,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }
   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   if(myrank==0)
   {
	std::vector<int> temp;
	temp.assign(op.begin(),op.begin()+numrecords);
	op.assign(temp.begin(),temp.end());
   }

   std::free(reqs);
}

void create_dataworld_testinput(std::string &filename,int numprocs,int myrank,std::vector<uint64_t> &keys,std::vector<uint64_t>&ts,std::vector<int>&op)
{
   hid_t xfer_plist = H5Pcreate(H5P_DATASET_XFER);
   hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
   H5Pset_fapl_mpio(fapl,MPI_COMM_WORLD, MPI_INFO_NULL);
   H5Pset_dxpl_mpio(xfer_plist, H5FD_MPIO_COLLECTIVE);

   hid_t fid = H5Fopen(filename.c_str(),H5F_ACC_RDONLY,fapl);

   hsize_t attr_size[1];
   attr_size[0] = MAXBLOCKS*4+4;
   const char *attrname[1];
   hid_t attr_space[1];
   attr_space[0] = H5Screate_simple(1, attr_size, NULL);

   attrname[0] = "Datasizes";

   std::string data_string = "Data1";
   hid_t dataset1 = H5Dopen2(fid,data_string.c_str(), H5P_DEFAULT);

   hid_t file_dataspace = H5Dget_space(dataset1);
   hid_t attr_id = H5Aopen(dataset1,attrname[0],H5P_DEFAULT);
   std::vector<uint64_t> attrs;
   attrs.resize(attr_size[0]);

   int ret = H5Aread(attr_id,H5T_NATIVE_UINT64,attrs.data());

   STRUCTEV(80);

   hsize_t adims[1];
   adims[0] = attrs[2];
   hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR,1,adims);
   hid_t s2 = H5Tcreate(H5T_COMPOUND,sizeof(struct keydata));
   if(s1< 0 || s2 < 0) std::cout <<" data types "<<std::endl;
   H5Tinsert(s2,"key",HOFFSET(struct keydata,ts),H5T_NATIVE_UINT64);
   H5Tinsert(s2,"value",HOFFSET(struct keydata,data),s1);

   hsize_t totalrecords = attrs[0];
   int records_per_proc = totalrecords/numprocs;
   int rem = totalrecords%numprocs;

   hsize_t offset=0;

   for(int i=0;i<myrank;i++)
   {
	int nrecords = 0;
	if(i < rem) nrecords = records_per_proc+1;
        else nrecords = records_per_proc;
	offset += nrecords;	
   }

   hsize_t blocksize = 0;
   if(myrank < rem) blocksize = records_per_proc+1;
   else blocksize = records_per_proc;

   std::vector<struct keydata> *buffer = new std::vector<struct keydata> ();
   buffer->resize(blocksize);

   ret = H5Sselect_hyperslab(file_dataspace, H5S_SELECT_SET,&offset,NULL,&blocksize,NULL);
   hid_t mem_dataspace = H5Screate_simple(1,&blocksize, NULL);
   ret = H5Dread(dataset1,s2, mem_dataspace, file_dataspace, xfer_plist,buffer->data());
   H5Sclose(mem_dataspace);

   for(int i=0;i<buffer->size();i++)
   {
	uint64_t tss = (*buffer)[i].ts;
	uint64_t vl = *(uint64_t*)(char *)((*buffer)[i].data);
	keys.push_back(vl);
	ts.push_back(tss);
	if(i%2==0) op.push_back(0);
	else op.push_back(1);
   }

   delete buffer;
  
   H5Tclose(s2);
   H5Tclose(s1);
   H5Sclose(attr_space[0]);
   H5Aclose(attr_id);
   H5Sclose(file_dataspace);
   H5Dclose(dataset1);
   H5Fclose(fid);   

}
