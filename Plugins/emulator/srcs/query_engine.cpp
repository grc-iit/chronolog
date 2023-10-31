#include "query_engine.h"

void query_engine::send_query(std::string &s)
{
       int count = query_number.fetch_add(1);
       struct query_req r;
       r.name = s;
       r.minkey = 0;
       r.maxkey = UINT64_MAX;
       r.collective = false;
       r.id = count;
       r.sorted = true;
       r.output_file = false;
       r.single_point = false;
       r.sender = myrank;
       Q->PutRequestAll(r);

}

void query_engine::query_point(std::string &s,uint64_t ts)
{
   int count = query_number.fetch_add(1); 
   struct query_req r;
   r.name = s;
   r.minkey = ts;
   r.maxkey = ts;
   r.collective = false;
   r.id = count;
   r.sender = myrank;
   r.from_nvme = true;
   r.sorted = false;
   r.output_file = false;
   r.single_point = true;
 
   int index = rwp->get_stream_index(s);

   if(index != -1)
   {
     int pid = rwp->get_event_proc(index,ts);



     if(pid != -1)
     {
       r.from_nvme = false;
       Q->PutRequest(r,pid);
     }
     else
    {
	pid = rwp->get_nvme_proc(s,ts);
	//if(pid==-1) pid=4;
	if(pid != -1)
	{
	   r.from_nvme = true;
	   Q->PutRequest(r,pid);
	}
	else
	{
           struct event *e = new struct event();
	   e->ts = 0;
	   char data[100];
	   e->data = data;
	   struct query_resp rp;
	   rp.id = r.id;
	   rp.response_id = 0;
	   rp.minkey = ts;
	   rp.sender = myrank;
	   rp.complete = true;
	   rp.response = pack_event(e,100);
	   rp.error_code = NOTFOUND;
	   Q->PutResponse(rp,r.sender);
	   delete e;
	}
    }
   }
   else
   {
	struct query_resp rp;
	rp.id = r.id;
	rp.response_id = 0; 
        rp.minkey = ts;
   	rp.maxkey = ts;
   	rp.sender = myrank;
   	rp.complete = true;	
   	rp.error_code = NOTFOUND;
	Q->PutResponse(rp,r.sender);
   }
}

void query_engine::sort_response(std::string &s,int id,std::vector<struct event> *buf,uint64_t &maxkey)
{
     /*int index1,index2;
     index1 = create_buffer(s,index2);

     boost::upgrade_lock<boost::shared_mutex> lk(sbuffers[index1]->m);
     {
	for(int i=0;i<buf->size();i++)
	    if((*buf)[i].ts > maxkey) sbuffers[index1]->buffer->push_back((*buf)[i]);
	ds->get_unsorted_data(sbuffers[index1]->buffer,index2);
	int tag = 10000+id;
	uint64_t minkey,maxkey;
	ds->sort_data(index2,tag,sbuffers[index1]->buffer->size(),minkey,maxkey);
	buf->assign(sbuffers[index1]->buffer->begin(),sbuffers[index1]->buffer->end());
	sbuffers[index1]->buffer->clear();
     }*/
}

bool query_engine::end_file_read(bool end_read,int id)
{
    /*int s_req = (end_read==true) ? 1 : 0;

    std::vector<int> r_req(numprocs);

    int tag = 10000+id;

    MPI_Request *reqs = (MPI_Request *)std::malloc(2*numprocs*sizeof(MPI_Request));

    int nreq = 0;

    for(int i=0;i<numprocs;i++)
    {
	MPI_Isend(&s_req,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&r_req[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
    }

    MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

    bool end_read_g = true;

    for(int i=0;i<numprocs;i++)
    {
	if(r_req[i]==0) end_read_g = false;
    }

    std::free(reqs);
    return end_read_g;*/
    return false;
}
void query_engine::get_range(std::vector<struct event> *buf1,std::vector<struct event> *buf2,std::vector<struct event> *buf3,uint64_t minkeys[3],uint64_t maxkeys[3],int id)
{
    /*int tag = 10000+id;
    MPI_Request *reqs = (MPI_Request*)std::malloc(3*numprocs*sizeof(MPI_Request));

    std::vector<uint64_t> send_ts; send_ts.resize(6);
    std::vector<uint64_t> recv_ts; recv_ts.resize(6*numprocs);
   
    send_ts[0] = UINT64_MAX; send_ts[2] = UINT64_MAX; send_ts[4] = UINT64_MAX;
    send_ts[1] = 0; send_ts[3] = 0; send_ts[5] = 0;

    if(buf1 != nullptr && buf1->size()>0)
    {
	send_ts[0] = (*buf1)[0].ts;
	send_ts[1] = (*buf1)[buf1->size()-1].ts;
    }

    if(buf2 != nullptr && buf2->size()>0)
    {
	send_ts[2] = (*buf2)[0].ts;
	send_ts[3] = (*buf2)[buf2->size()-1].ts;
    }

    if(buf3 != nullptr && buf3->size()>0)
    {
	send_ts[4] = (*buf3)[0].ts;
	send_ts[5] = (*buf3)[buf3->size()-1].ts;
    }

    int nreq=0;

    for(int i=0;i<numprocs;i++)
    {
	MPI_Isend(send_ts.data(),6,MPI_UINT64_T,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recv_ts[6*i],6,MPI_UINT64_T,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
    }

    MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

    minkeys[0] = UINT64_MAX; minkeys[1] = UINT64_MAX; minkeys[2] = UINT64_MAX;
    maxkeys[0] = 0; maxkeys[1] = 0; maxkeys[2] = 0;

    for(int i=0;i<numprocs;i++)
    {
	if(minkeys[0] > recv_ts[6*i]) minkeys[0] = recv_ts[6*i];
	if(maxkeys[0] < recv_ts[6*i+1]) maxkeys[0] = recv_ts[6*i+1];
	if(minkeys[1] > recv_ts[6*i+2]) minkeys[1] = recv_ts[6*i+2];
	if(maxkeys[1] < recv_ts[6*i+3]) maxkeys[1] = recv_ts[6*i+3];
	if(minkeys[2] > recv_ts[6*i+4]) minkeys[2] = recv_ts[6*i+4];
	if(maxkeys[2] < recv_ts[6*i+5]) maxkeys[2] = recv_ts[6*i+5];
    }

     std::free(reqs);*/
}

int query_engine::count_end_of_session()
{
   int send_v = 0;
   std::vector<int> recv_v(numprocs);
   std::fill(recv_v.begin(),recv_v.end(),0);

   if(Q->EmptyRequestQueue()) send_v = 1;   

   MPI_Request *reqs = new MPI_Request[2*numprocs];
   int tag = 10000;

   int nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
     MPI_Isend(&send_v,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
     nreq++;
     MPI_Irecv(&recv_v[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
     nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   std::free(reqs);

   int nend = 0;
   for(int i=0;i<recv_v.size();i++)
      nend+=recv_v[i];

   return nend;
}

void query_engine::service_response(struct thread_arg_q *t)
{

  while(true)
  {

	struct query_resp *r = nullptr;
	r = Q->GetResponse();
	if(r != nullptr)
	{
		delete r;
	}

	if(Q->EmptyResponseQueue() && end_request.load()==1) break;
  }

}


void query_engine::service_query(struct thread_arg_q* t) 
{
	int end_service = 0;

	int endsessions=0;

	bool sorted = false;

	while(true)
	{
	   if(end_session.load()==1)
	   {
		int nend = count_end_of_session();
		if(nend==numprocs) 
		{
		   //end_request.store(1);
		   break;
		}

	   }
	}

	rwp->end_qe();

	   /*if(endsessions==numprocs && end_session.load()==0)
	   {
		end_session.store(1);
	   } 
	   struct query_req *r = nullptr;
	   r=Q->GetRequest();
	   if(r != nullptr)
          {

	      std::string sname(r->name);
	      if(sname.compare("endsession")==0) 
	      {
		     endsessions++;
		     delete r;
	      }
	      else
	      {
	        uint64_t min_key1,max_key1;

	         bool b = false;
	    
	         int pid = -1;

		 uint64_t ts = r->minkey;
	         int index = rwp->get_stream_index(r->name);
		 pid = rwp->get_event_proc(index,ts);
		 if(pid == myrank)
		 {
	            event_metadata em = rwp->get_metadata(r->name);
	            struct event *e = new struct event();
		    e->ts = 0;
		    e->data = nullptr;
	            if(index != -1)
	            { 
		       e->data = new char[em.get_datasize()];
	               if(!r->from_nvme)
		       {
	                  b = false; //rwp->find_event(r->name,r->minkey,e);
		          if(!b)
		          {
			    pid = rwp->get_nvme_proc(r->name,r->minkey);
			    if(pid != -1)
			    {
			      r->from_nvme = true;
			      Q->PutRequest(*r,pid);
			    }
		          }
		       }
		    }*/

	             /* else
		  {
	            b = rwp->find_nvme_event(r->name,r->minkey,e);
		  }
		 }

		  if(b || (r->from_nvme==true && !b))
		  {
		     struct query_resp s;
		     s.id = r->id;
		     s.minkey = r->minkey;
		     s.maxkey = r->maxkey;
		     s.sender = myrank;
		     s.response = pack_event(e,em.get_datasize()); 
		     s.complete = true;
		     int dest = r->sender;
		     Q->PutResponse(s,dest);
		  }*/
		  /*if(e->data != nullptr) delete e->data;
		  delete e;
	         }
	         else if(pid != -1 && index != -1)
		 {	
		    Q->PutRequest(*r,pid);

		 }
		  delete r;
		}*/
	          
		/*pid = r->sender;
	     
	        struct query_resp s;
	     
	        s.id = r->id;
   	        s.minkey = r->minkey;
   	        s.maxkey = r->maxkey;
   	        s.sender = myrank;
   	        //s.response = e.pack_event();
   	        s.complete = true;
  		
	        std::cout <<" ts = "<<e.ts<<std::endl; 
	        //Q->PutResponse(s,pid); */
	     //}
	     
	      /*std::vector<struct event> *buf1 = new std::vector<struct event> ();
	      std::vector<struct event> *buf2 = new std::vector<struct event> ();
	      std::vector<struct event> *buf3 = new std::vector<struct event> ();
	      std::vector<struct event> *resp_vec = nullptr;


	      std::string filename = "file";
	      //filename += names[i]+".h5";
                filename += sname+".h5";

	      uint64_t minkey_f,maxkey_f;

	      uint64_t minkey_r,maxkey_r;

	      uint64_t minkey_e = 0; 
	      uint64_t maxkey_e = UINT64_MAX;
	
		  
	      struct io_request *nq = new struct io_request();
	      nq->name.assign(sname);
	      nq->completed.store(0);
	      nq->buf1 = buf1;
	      nq->buf2 = buf2;
	      nq->buf3 = buf3;
	      rwp->sync_queue_push(nq);
	      while(!nq->completed.load()==1);

	      delete nq;

	      uint64_t minkeys[3],maxkeys[3];
	      
	      get_range(buf1,buf2,buf3,minkeys,maxkeys,100);

	      if(myrank==0)
              {
         	std::cout <<" id = "<<r->id<<" minkey 3 = "<<minkeys[2]<<" maxkey 3 = "<<maxkeys[2]<<std::endl;
         	std::cout <<" id = "<<r->id<<" minkey 2 = "<<minkeys[1]<<" maxkey 2 = "<<maxkeys[1]<<std::endl;
         	std::cout <<" id = "<<r->id<<" minkey 1 = "<<minkeys[0]<<" maxkey 1 = "<<maxkeys[0]<<std::endl;
     	      }*/


	      /*if(sorted)
	      {

		  uint64_t maxkey = std::max(maxkeys[1],maxkeys[2]);
		  sort_response(names[i],i,buf1,maxkey);
	      }*/
	        
	      /*resp_vec = new std::vector<struct event> ();

	      uint64_t minkey = UINT64_MAX;
	      uint64_t maxkey = 0;
	      if(buf3 != nullptr) 
	      {
		  resp_vec->assign(buf3->begin(),buf3->end()); buf3->clear();
	      }

	      if(buf2 != nullptr)
	      {
		  for(int i=0;i<buf2->size();i++)
	      	  {
		   if((*buf2)[i].ts > maxkeys[2]) 
	           {
		     resp_vec->push_back((*buf2)[i]);
		   }
		 }
		 buf2->clear();			
	      }

	      if(buf1 != nullptr)
	      {
		for(int i=0;i<buf1->size();i++)
		{
		   if((*buf1)[i].ts > maxkeys[2] && (*buf1)[i].ts > maxkeys[1])
		   {
			resp_vec->push_back((*buf1)[i]);
		   }
		}
		buf1->clear();
	       }*/
      	      /*struct query_resp *p = new struct query_resp();

	      p->response_vector = nullptr;
	      p->response_vector = resp_vec;

	      O->push(p);	 */   

	      //delete buf1; 
	      //delete buf2;
	      //delete buf3;
	      //delete resp_vec;
           //}
	//}

}

void query_engine::sort_file(std::string &s)
{

    /*  std::string attrname = "attr"+std::to_string(0);
      std::string attr_type = "integer";
      std::string outputfile = hs->sort_on_secondary_key<int>(s,attrname,0,0,UINT64_MAX,attr_type);

      hs->merge_tree<int>(s,0);*/
}
