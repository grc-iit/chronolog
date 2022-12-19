#ifndef CHRONOLOG_KEEPER_IDCARD_H
#define CHRONOLOG_KEEPER_IDCARD_H

// this class wrapps ChronoKeeper Process identification 
// that will be used internally and exposed to the client

namespace chronolog
{

// Keeper Process can be uniquely identified by the combination of
// the host IP address + client_port
//INNA: another candidate: IP address of the host + process id of the KeeperProcess

typedef uint32_t 	in_addr_t;
typedef uint16_t	in_port_t;
typedef std::pair <in_addr_t, in_port_t> KeeperProcessId;
 
// KeeperGroup is the logical grouping of KeeperProcesses
typedef std::string    KeeperGroupId;

class KeeperIdCard
{
public:
	KeeperIdCard(KeeperGroupId cosnt& keeper_group, KeeperProcessId cosnt& keeper_pid)
		: keeperGroupId{keeper_instance}, keeperProcessId{keeper_pid}
		{ }
	KeeperIdCard(KeeperIdCard cosnt& other)
		: keeperGroupId{other.getKeeperGroupId()}, keeperProcessId{other.getKeeperProcessId()}
		{ }

	~KeeperIdCard() = default;

	KeeperGroupId const& getKeeperGroupId() const
	{	return keeperGroupId}
	KeeperProcessId const& getKeeperProcessId() const
	{	return keeperProcessId}

	std::pair<in_addr_t,in_port_t> const& getIPandPort() const
	{	return keeperProcessId; }      
private:
	KeeperGroupId	keeperGroupId;
	KeeperProcessId	keeperProcessId; 

};

}//namespace cronolog

std::ostream & operator<< (std::ostream & out, chronolog::KeeperProcessId const& keeperProcessId)
{
	//INNA: convert in_ip to string for pretty printing
	out<<"KeeperPid{"<<keeperProcessId.first <<":"<< keeperProcessId.second<<"}";
   return out;
}
std::ostream & operator<< (std::ostream & out, chronolog::KeeperProcessIdCard const& keeperIdCard)
{
	out<<"KeeperIdCard{"<<keeperIdCard.getKeeperGroupId()<<":"<<keeperIdCard.getkeeperProcessId()<<"}";
   return out;
}

#endif CHRONOLOG_KEEPER_IDCARD_H
