#ifndef STRORYTELLER_CLIENT_H
#define STROYTELLER_CLIENT_H


namespace chronolog



template <typename ClientId,
	  typename ChronoTime,
	  typename StorytellingToken,
          typename StoryMetadata,
          typename RPCChannel>
class Storyteller
{
public:
	Storyteller( ClientId const&);
	~Storyteller();

	StoryAccessToken const & getStoryAccessToken ( StoryId const&) const;
	ChronoTime	const& getStorytellerTime (ClientId const&) const;
	void 		updateStorytellerTime ( ChronoTime cosnt&);
	StoryMetadata const&  beginStoryRecording( StoryId const&, StoryAccessToken cosnt&);
	StoryMetadata const&  endStoryRecording( StoryId const&);
 
private:
	ClientId clientId; 
	ChronoTime clientTime;  
	RPCChannel rpcChannel;
	std::map < StoryId, std::pair<StorytellingToken, StoryMetadata> > activeStories;

};

} //namespace chronolog





#endif STORYTELLER_CLIENT_H
