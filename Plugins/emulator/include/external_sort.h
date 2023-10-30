#ifndef __EXTERNAL_SORT_H_
#define __EXTERNAL_SORT_H_

#include "rw.h"


class hdf5_sort
{
   private: 
	int numprocs;
	int myrank;
	MPI_Comm merge_comm;


  public: 
       hdf5_sort(int n,int p) : numprocs(n), myrank(p)
       {
	  MPI_Comm_dup(MPI_COMM_WORLD,&merge_comm);

       }
       ~hdf5_sort()
       {
	  MPI_Comm_free(&merge_comm);
       }

       template<typename T>
       void merge_tree(std::string&,int);
       template<typename T,int M>
       void count_offset(std::vector<struct event>*,std::vector<struct event>*,int&,int,int&,int,T&,bool&);
       template<typename T,int M>
       int insert_block(std::vector<struct event>*,std::vector<struct event>*,std::vector<struct event> *,int,int,int&,T&,T&);
       template<typename T,int M>
       void sort_block_secondary_key(std::vector<struct event>*,int,int,T&,T&,int&);
       template<typename T>
       std::string sort_on_secondary_key(std::string &,std::string &,int,uint64_t,uint64_t,std::string &);
       std::string merge_datasets(std::string &,std::string &);
       std::string merge_stream_with_dataset(std::string &,std::vector<struct event>*);
       std::string merge_multiple_dataset(std::vector<std::string>&);
};

#include "../srcs/external_sort.cpp"

#endif
