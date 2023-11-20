//
// Created by kfeng on 10/25/21.
//

#include <SocketPP.h>
#include <ChronoVisorServer.h>

#define NUM_CHRONOKEEPER 32
//#define NDEBUG 1

using namespace ChronoVisor;

int main()
{
    const int port = 6000;
    LOGI("ChronoVisor server port:%d", port);

    ChronoVisorServer server(port, 10);
    server.setClocksourceType(ClocksourceType::C_STYLE);

//    server.setConnHandle([] (const SocketPP::TCPStream& stream) {
//        LOGI("a client connected: fd=%d", stream.fd);
//    });
//
//    server.setDiscHandle([] (const SocketPP::TCPStream& stream) {
//        LOGI("a client disconnected: fd=%d", stream.fd);
//    });
//
//    server.setRecvHandle([&] (const SocketPP::Message& message) {
//        LOGI("received a message from a client: msg=%s", message.rawMsg.toString().c_str());
//    });

    for(int i = 0; i < NUM_CHRONOKEEPER; i++)
    {
        ChronoKeeperInfo chronoKeeperInfo(std::string("172.25.101.") + std::to_string(i + 1), 8000 + i);
        server.addChronoKeeperInfo(chronoKeeperInfo);
    }

    return server.start();
}

