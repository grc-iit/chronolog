#include <vector>
#include <iostream>
#include <sstream>
#include <hdf5.h>
#include "h5_async_lib.h"
#include <string>
#include <cfloat>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <chrono>
#include <ctime>
#include <mpi.h>

struct keydata
{
   uint64_t ts; 
   char *data;
};

int main(int argc,char **argv)
{
   std::string filename = "covid.csv";


   std::string hfile = "covid.h5";

   int prov;

   MPI_Init_thread(&argc,&argv,MPI_THREAD_SINGLE,&prov);

   std::ifstream ist(filename.c_str(),std::ios_base::in);

   std::vector<std::string> lines;

   std::vector<uint64_t> ts;
   std::vector<std::vector<uint64_t>> values;

   std::vector<std::string> attributes;

   if(ist.is_open())
   {
     std::string line;
     while(getline(ist,line))
     {
	
	lines.push_back(line);
     }

     std::cout <<" numlines = "<<lines.size()<<std::endl;

     time_t rawtime;

     for(int i=0;i<lines.size();i++)
     {
       const auto p0 = std::chrono::time_point<std::chrono::system_clock>{};

       std::time_t epoch_time = std::chrono::system_clock::to_time_t(p0);


       char *t = strtok((char*)lines[i].c_str(),",");

       
       if(t != nullptr)
       {
         std::string d(t);

	 if(d.empty()||d.length()==1) 
	 {
		 break;
	 }

	 if(i >= 1)
	 {
           int pos1 = d.find("/");
           std::string day = d.substr(0,pos1);
           std::string month = d.substr(pos1+1);
           int pos2 = month.find("/");
           std::string mon = month.substr(0,pos2);
           std::string year = month.substr(pos2+1);

           int day_n = std::stoi(day);
           int mon_n = std::stoi(mon);
           int year_n = std::stoi(year);
	 
	   time ( &rawtime );
           struct tm *timeinfo = localtime ( &rawtime );

           timeinfo->tm_year = year_n-1900;
           timeinfo->tm_mon = mon_n-1;
           timeinfo->tm_mday = day_n;
           timeinfo->tm_hour = 0;
           timeinfo->tm_min = 0;
           timeinfo->tm_isdst = 0;

           std::time_t time_1 = std::mktime(timeinfo);

           std::chrono::time_point<std::chrono::system_clock> t1 = std::chrono::system_clock::from_time_t(time_1);

	   uint64_t ts1 = t1.time_since_epoch().count();

	   ts.push_back(ts1);

	   std::chrono::time_point<std::chrono::system_clock> t2 = std::chrono::system_clock::now();
	   uint64_t ts2 = t2.time_since_epoch().count();
	 }
	  
	 std::vector<uint64_t> row;

           while(t != nullptr)
          {
	   if(t != nullptr && i==0) attributes.push_back(std::string(t));
	   t = strtok(NULL,",");
	   if(t != nullptr && i!=0)
	   {
		std::string stringv(t);
		uint64_t v = std::stoul(stringv,nullptr,0);
		row.push_back(v);
	   }
          }
	  if(i !=0) values.push_back(row);
       }

     }
     ist.close();
   }

  std::cout <<" ts = "<<ts.size()<<" values = "<<values.size()<<std::endl;

  std::vector<std::pair<uint64_t,uint64_t>> ranges;
  std::vector<int> numrecords;

  hid_t async_fapl = H5Pcreate(H5P_FILE_ACCESS);
  hid_t async_dxpl = H5Pcreate(H5P_DATASET_XFER);

  H5Pset_fapl_mpio(async_fapl, MPI_COMM_WORLD, MPI_INFO_NULL);
  H5Pset_dxpl_mpio(async_dxpl, H5FD_MPIO_COLLECTIVE);

  int numattributes = attributes.size()-1;
  int bytes_per_attribute = 8;
  int valuesize = numattributes*bytes_per_attribute;

  int keydatasize = sizeof(uint64_t)+valuesize*sizeof(char);
  char *data_p = (char*)malloc(keydatasize*ts.size());

  uint64_t minkey = 0;
  uint64_t maxkey = 0;
  int c = -1;
  int n = 0;
  for(int i=0;i<ts.size();i++)
  {	
	*(uint64_t*)(&data_p[i*keydatasize]) = ts[i];
	std::memcpy(&data_p[i*keydatasize+sizeof(uint64_t)],reinterpret_cast<char*>(values[i].data()),valuesize);
	if(n==0) 
	{
            c++;
	    minkey = ts[i];n++;
	}
	else if(n > 0 && n%8192==0)
	{
	    maxkey = ts[i]; n=0;
	    std::pair<uint64_t,uint64_t> p;
	    p.first = minkey; p.second = maxkey;
	    ranges.push_back(p);
	    numrecords.push_back(8192);
	}
	else n++;
  }

  if(n%8192!=0)  
  {
    maxkey = ts[ts.size()-1];
    std::pair<uint64_t,uint64_t> p;
    p.first = minkey; p.second = maxkey;
    ranges.push_back(p);
    numrecords.push_back(n);
  }

  const char *attr_name[1];
  hsize_t adims[1];
  adims[0] = (hsize_t)valuesize;
  hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR,1,adims);
  hid_t s2 = H5Tcreate(H5T_COMPOUND,keydatasize);
  hid_t cstr_id = H5Tcopy(H5T_C_S1);
  H5Tset_size(cstr_id, H5T_VARIABLE);

  H5Tinsert(s2,"key",HOFFSET(struct keydata,ts),H5T_NATIVE_UINT64);
  H5Tinsert(s2, "value", HOFFSET(struct keydata,data),s1);

  hsize_t attr_size[1];
  attr_size[0] = MAXBLOCKS*4+4;
  hid_t attr_space[1];
  attr_name[0] = "Datasizes";
  attr_space[0] = H5Screate_simple(1, attr_size, NULL);

  std::vector<uint64_t> attr_data;
  attr_data.resize(attr_size[0]);
  hsize_t total_records = (hsize_t)ts.size();

  attr_data[0] = total_records;
  attr_data[1] = 8;
  attr_data[2] = valuesize;
  attr_data[3] = numrecords.size();

  int pos = 4;
  for(int i=0;i<numrecords.size();i++)
  {
     attr_data[pos+i*4] = ranges[i].first;
     attr_data[pos+i*4+1] = ranges[i].second;
     attr_data[pos+i*4+2] = i;
     attr_data[pos+i*4+3] = numrecords[i];
  }


  hsize_t chunkdims[1];
  chunkdims[0] = total_records;
  hsize_t maxdims[1];
  maxdims[0] = (hsize_t)H5S_UNLIMITED;

  hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);
  int ret = H5Pset_chunk(dataset_pl,1,chunkdims);
  hid_t file_dataspace = H5Screate_simple(1,&total_records,maxdims);
  std::string dstring = "Data1";
  hid_t fid = H5Fcreate(hfile.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, async_fapl);

  hid_t dataset1 = H5Dcreate(fid,dstring.c_str(),s2,file_dataspace, H5P_DEFAULT,dataset_pl, H5P_DEFAULT);
  hid_t mem_dataspace = H5Screate_simple(1,&total_records,NULL);

  hsize_t offset_w = 0;

  ret = H5Sselect_hyperslab(file_dataspace,H5S_SELECT_SET,&offset_w,NULL,&total_records,NULL);
  ret = H5Dwrite(dataset1,s2,mem_dataspace,file_dataspace,async_dxpl,data_p);

  hid_t attr_id[1];
  attr_id[0] = H5Acreate(dataset1, attr_name[0], H5T_NATIVE_UINT64, attr_space[0], H5P_DEFAULT, H5P_DEFAULT);
  ret = H5Awrite(attr_id[0], H5T_NATIVE_UINT64, attr_data.data());
  ret = H5Aclose(attr_id[0]);

  free(data_p);
  //delete data_p;
  H5Pclose(dataset_pl);
  H5Sclose(attr_space[0]);
  H5Tclose(s2);
  H5Tclose(cstr_id);
  H5Tclose(s1);
  H5Sclose(file_dataspace);
  H5Sclose(mem_dataspace);
  H5Dclose(dataset1);
  H5Fclose(fid);

  MPI_Finalize();



}
