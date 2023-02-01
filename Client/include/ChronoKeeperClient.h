#ifndef CHRONOLOG_KEEPER_CLIENT_H
#define CHRONOLOG_KEEPER_CLIENT_H

#include <vector>
#include <map>

#include <ChronoKeeperIdCard.h>

namespace chronolog
{

template <typename RPCChannel>
class ChronoKeeperClient
{
public:
	ChronoKeeperClient( std::vector<KeeperIdCard> const&);
	~ChronoKeeperClient();

	void AppendLogRecord( void* data, size_t data_size);
	
private:
        //ip*port pair for sending events to the specific keeper process 
	// is incoporated into the KeeprIdCard 
	std::map< KeeperProcessId, std::pair<KeeperIdCard,RPCChannel> > keeperRPCChannels;
};



}//namespace cronolog

#endif CHRONOLOG_KEEPER_CLIENT_H
