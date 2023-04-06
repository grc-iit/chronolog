#ifndef CHRONOLOG_STORYHANDLE_H
#define CHRONOLOG_STORYHANDLE_H

namespace chronolog
{

//Abstract StoryHandle will be returnrned to the cleitn in AcquireStory() API call
class StoryHandle
{
public:
    virtual  ~StoryHandle();

    virtual int log_event( std::string const&) = 0;

    // to be implemented with libfabric/thallium bulk transfer...
    //virtual int log_event( size_t , void*) = 0;
};

class StorytellerClient;

// top level Chronolog Client...
// implementation details are in the StorytellerClient class 
class Client
{
public:
	Client();
	~Client();
	//ClientId const& Connect( ClientConfig const&);
	StoryHandle *  AcquireStory( ChronicleName const&, StoryName const&); //, AccessType);
	
private:
	StorytellerClient * storyteller;
};

} //namespace chronolog

#endif
