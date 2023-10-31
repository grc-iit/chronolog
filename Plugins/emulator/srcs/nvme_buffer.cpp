#include "nvme_buffer.h"
#include <mpi.h>

void nvme_buffers::create_nvme_buffer(std::string &s,event_metadata &em)
{
     std::string fname = prefix+s;
     auto r = nvme_fnames.find(fname);

     if(r == nvme_fnames.end())
     {
          file_mapping::remove(fname.c_str());
          int maxsize = 65536*VALUESIZE;
          managed_mapped_file *mf = new managed_mapped_file(create_only,fname.c_str(),maxsize);
          const allocator_event_t allocator_e(mf->get_segment_manager());
	  const allocator_char_t allocator_c(mf->get_segment_manager());
          std::string vecname = fname+"MyEventVector";
	  std::string vecdname = fname+"MyEventDataVector";
          MyEventVect *ev = mf->construct<MyEventVect> (vecname.c_str()) (allocator_e);
	  MyEventDataVect *ed = mf->construct<MyEventDataVect> (vecdname.c_str()) (allocator_c);
	 if(mf->find<MyEventVect>(vecname.c_str()).first != ev ||
	    mf->find<MyEventDataVect>(vecdname.c_str()).first != ed)
	 {
		throw std::runtime_error("nvme memory not mapped");
		return;
	 } 
          boost::shared_mutex *m = new boost::shared_mutex();
          file_locks.push_back(m);
          nvme_ebufs.push_back(ev);
	  nvme_dbufs.push_back(ed);
          nvme_files.push_back(mf);
          file_names.push_back(fname);
          buffer_names.push_back(vecname);
	  bufferd_names.push_back(vecdname);
          std::pair<std::string,std::pair<int,event_metadata>> p2;
	  p2.first.assign(fname);
	  boost::mutex *mock = new boost::mutex();
	  p2.second.first = file_names.size()-1;
	  p2.second.second = em;
          nvme_fnames.insert(p2);
	  blocks.push_back(mock);
	  std::atomic<int> *bs = (std::atomic<int>*)std::malloc(sizeof(std::atomic<int>));
	  bs->store(0);
	  buffer_state.push_back(bs);
	  total_blocks[file_names.size()-1] = 0;
      }
}

void nvme_buffers::copy_to_nvme(std::string &s,std::vector<struct event> *inp,int numevents)
{
    std::string fname = prefix+s;
    auto r = nvme_fnames.find(fname);

    if(r == nvme_fnames.end()) return;

    int index = r->second.first;
    event_metadata em = (r->second).second;
    int datasize = em.get_datasize();

    MyEventVect *ev = nvme_ebufs[index];
    MyEventDataVect *ed = nvme_dbufs[index];

    int psize = ev->size();
    int psized = ed->size();

    if(numevents > 0)
    {
      try
      {
        ev->resize(psize+numevents);
        ed->resize(psized+numevents*datasize);
      }
      catch(const std::exception &except)
      {
	std::cout <<except.what()<<std::endl;
	exit(-1);
      }
    }

    int p = psize;
    int pd = psized;

    uint64_t mints = UINT64_MAX;
    uint64_t maxts = 0;

    if(numevents > 0)
    {
	int len = numevents-1;
	mints = (*inp)[0].ts;
	maxts = (*inp)[len].ts;
    }

    for(int i=0;i<numevents;i++)
    {
      uint64_t ts = (*inp)[i].ts;
      (*ev)[p].ts = ts;
      std::memcpy((&((*ed)[pd])),(*inp)[i].data,datasize);
      (*ev)[p].data = (&((*ed)[pd]));
      p++;
      pd += datasize;
    }

    nvme_files[index]->flush();

    add_block(index,numevents);

    update_interval(index,mints,maxts);
}

void nvme_buffers::add_block(int index,int numevents)
{
   MPI_Request *reqs = new MPI_Request[2*numprocs];
   assert (reqs != nullptr);

   int sendlen = numevents;
   std::vector<int> recvlens(numprocs);

   std::fill(recvlens.begin(),recvlens.end(),0);

   int nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
	MPI_Isend(&sendlen,1,MPI_INT,i,index,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recvlens[i],1,MPI_INT,i,index,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   numblocks[index].push_back(recvlens);

   total_blocks[index]++;

   delete reqs;
}

void nvme_buffers::erase_from_nvme(std::string &s, int numevents,int nblocks)
{
      std::string fname = prefix+s;
      auto r = nvme_fnames.find(fname);
      event_metadata em = (r->second).second;

      int index = r->second.first;

      MyEventVect *ev = nvme_ebufs[index];
      MyEventDataVect *ed = nvme_dbufs[index];
      int datasize = em.get_datasize();

      try
      {
        ev->erase(ev->begin(),ev->begin()+numevents);
        ed->erase(ed->begin(),ed->begin()+(numevents*datasize));
      }
      catch(const std::exception &except)
      {
	std::cout <<except.what()<<std::endl;
	exit(-1);
      }


      nvme_files[index]->flush();

      remove_blocks(index,nblocks);

      //update_interval(index);

}

void nvme_buffers::remove_blocks(int index,int nc)
{
   total_blocks[index] -= nc;

   std::vector<std::vector<int>> temp;

   for(int i=nc;i<numblocks[index].size();i++)
   {
      temp.push_back(numblocks[index][i]);
   }

   numblocks[index].clear();

   for(int i=0;i<temp.size();i++)
      numblocks[index].push_back(temp[i]);

   std::vector<std::vector<std::pair<uint64_t,uint64_t>>> temp_i;

   for(int i=nc;i<nvme_intervals[index].size();i++)
   {
	   std::vector<std::pair<uint64_t,uint64_t>> block_range(numprocs);
	   block_range.assign(nvme_intervals[index][i].begin(),nvme_intervals[index][i].end());
	   temp_i.push_back(block_range);
   }
   nvme_intervals[index].clear();

   for(int i=0;i<temp_i.size();i++)
	   nvme_intervals[index].push_back(temp_i[i]);

}

void nvme_buffers::update_interval(int index,uint64_t mints,uint64_t maxts)
{
   int nreq = 0;

   MPI_Request *reqs = new MPI_Request[2*numprocs];
   assert(reqs != nullptr);

   MyEventVect *ev = nvme_ebufs[index];

   std::vector<uint64_t> send_range(2);
   send_range[0] = mints;
   send_range[1] = maxts;

   std::vector<uint64_t> recv_ranges(2*numprocs);

   for(int i=0;i<numprocs;i++)
   {
	MPI_Isend(send_range.data(),2,MPI_UINT64_T,i,index,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recv_ranges[2*i],2,MPI_UINT64_T,i,index,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   std::vector<std::pair<uint64_t,uint64_t>> recvranges(numprocs);

   for(int i=0;i<numprocs;i++)
   {
	recvranges[i].first = recv_ranges[2*i];
	recvranges[i].second = recv_ranges[2*i+1];
   }

   nvme_intervals[index].push_back(recvranges);

   delete reqs;
}

bool nvme_buffers::get_buffer(int index,int tag,int type)
{
   MPI_Request *reqs = new MPI_Request[2*numprocs];
   assert(reqs != nullptr);

   int nreq = 0;

   int s_req = type;
   int op;

   int m_tag = tag;
   int prev_value = 0;
   int next_value = type;


   if(myrank==0)
   {
      do
      {
	prev_value = 0;
	next_value = type;
      }while(!buffer_state[index]->compare_exchange_strong(prev_value,next_value));
      
      for(int i=1;i<numprocs;i++)
      {
	MPI_Isend(&s_req,1,MPI_INT,i,m_tag,MPI_COMM_WORLD,&reqs[nreq]);	
	nreq++;

      }
   }
   else
   {
	
	int r_type = 0;
	MPI_Irecv(&r_type,1,MPI_INT,0,m_tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   if(myrank != 0)
   {
	do
	{
	   prev_value = 0;
	   next_value = type;
	}while(!buffer_state[index]->compare_exchange_strong(prev_value,next_value));
   }

   int send_req = 1;
   std::vector<int> recv_req(numprocs);
   std::fill(recv_req.begin(),recv_req.end(),0);

   nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
	MPI_Isend(&send_req,1,MPI_INT,i,m_tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recv_req[i],1,MPI_INT,i,m_tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   delete reqs;
   return true;

}

int nvme_buffers::buffer_index(std::string &s)
{
    std::string fname = prefix+s;
    auto r = nvme_fnames.find(fname);

    int index;

    if(r == nvme_fnames.end()) index = -1;
    else index = r->second.first;

    return index;
}

void nvme_buffers::release_buffer(int index)
{
     /*if(myrank==0) 
     {
	  blocks[index]->unlock();
     }*/
     buffer_state[index]->store(0);

}

int nvme_buffers::get_proc(std::string &s,uint64_t ts)
{
   int index = buffer_index(s);

   int pid = -1;

   int prev_value=0;
   int new_value=4;

   do
   {
	prev_value = 0;
	new_value = 4;
   }while(!buffer_state[index]->compare_exchange_strong(prev_value,new_value));


   for(int i=0;i<nvme_intervals[index].size();i++)
   {
       bool found = false;
       for(int j=0;j<numprocs;j++)
       {
          if(ts >= nvme_intervals[index][i][j].first &&
	   ts <= nvme_intervals[index][i][j].second)
          {
	    pid = j; found = true; break;
          }
        }
        if(found) break;
   }

   buffer_state[index]->store(0);

   return pid;

}

bool nvme_buffers::find_event(std::string &s,uint64_t ts,struct event *e)
{

   std::string fname = prefix+s;
   auto r = nvme_fnames.find(fname);

   int index = -1;
   event_metadata em;
   if(r != nvme_fnames.end())
   {
      index = (r->second).first;
      em = (r->second).second;
   }

   int prev_value = 0;
   int new_value = 4;

   do
   {
	prev_value = 0;
	new_value = 4;
   }while(!buffer_state[index]->compare_exchange_strong(prev_value,new_value));

   bool ret = false;

   MyEventVect *ev = nvme_ebufs[index];

   e->ts = UINT64_MAX;
   int datasize = em.get_datasize();

   for(int i=0;i<ev->size();i++)
   {
	if((*ev)[i].ts==ts)
	{
	   e->ts = (*ev)[i].ts;
	   std::memcpy(e->data,(*ev)[i].data,datasize);
	   ret = true;
	   break; 
	}
   }

   buffer_state[index]->store(0);
   return ret;

}

void nvme_buffers::fetch_buffer(std::vector<char> *data_mem,std::string &s,int &index, int &tag,int &bc,std::vector<std::vector<int>> &blockcounts)
{

     std::string fname = prefix+s;
     auto r = nvme_fnames.find(fname);
     event_metadata em = (r->second).second;
     int datasize = em.get_datasize();

     index = r->second.first;

     MyEventVect *ev = nvme_ebufs[index];

     int keyvaluesize = sizeof(uint64_t)+datasize*sizeof(char);

     try
     {
       data_mem->resize(ev->size()*keyvaluesize);
     }
     catch(const std::exception &except)
     {
	std::cout << except.what()<<std::endl;
	exit(-1);
     }
     
     int p = 0;
     for(int i=0;i<ev->size();i++)
     {
         *(uint64_t*)(&((*data_mem)[p])) = (*ev)[i].ts;
	 p+=sizeof(uint64_t);
	 std::memcpy((&((*data_mem)[p])),(*ev)[i].data,datasize);
	 p+=datasize;
     }

     bc = total_blocks[index];

     for(int i=0;i<numblocks[index].size();i++)
     {
	blockcounts.push_back(numblocks[index][i]);
     }

     //nvme_ebufs[index]->clear();
          //nvme_files[index]->flush();

     //buffer_state[index]->store(0);

}

