#ifndef QUERY_RESPONSE_CLIENT_H
#define QUERY_RESPONSE_CLIENT_H


#include "StoryChunkExtractorRDMA.h"

namespace chronolog
{

 //INNA: use StoryChunkExtractorRDMA implementation to send the query response StoryChunks to the requesting client
 // listening Service address...

typedef StoryChunkExtractorRDMA QueryResponseClient;
}

#endif
