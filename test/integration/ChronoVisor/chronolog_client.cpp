//
// Created by kfeng on 11/15/21.
//

#include <chrono>
#include "SocketPP.h"
#include "SerDe.h"

//#define NDEBUG 1

using namespace SocketPP;

int main()
{
    const int port = 6000;
    TCPClient client("127.0.0.1", port);

    std::chrono::high_resolution_clock::time_point t1, t2;
    std::chrono::duration <double> duration{};
    SerDeFactory*pSerDeFactory = new SerDeFactory(SerDeType::CEREAL);
    SerDe*pSerDe = pSerDeFactory->getSerDe();

    client.setConnHandle([&](const TCPStream &stream)
                         {
                             ClientMessage clientMsg;
                             clientMsg.msgType_ = ClientMessage::CONNECTION;
                             std::unique_ptr <std::ostringstream> oss;
                             {
                                 oss = pSerDe->serializeClientMessage(clientMsg);
                             }
                             Logger::getLogger()->debug("[ChronoLog Client] Serialized client msg (len: {}): {}"
                                                        , oss->str().length(), oss->str().c_str());
                             t1 = std::chrono::high_resolution_clock::now();
                             client.send(oss->str());
                         });

    client.setRecvHandle([&](const Message &message)
                         {
                             std::unique_ptr <ServerMessage> serverMsg;
                             {
                                 serverMsg = pSerDe->deserializeServerMessage(message.rawMsg.data()
                                                                              , message.rawMsg.length());
                             }
                             t2 = std::chrono::high_resolution_clock::now();
                             duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
                             Logger::getLogger()->debug("[ChronoLog Client] Server message received: {}"
                                                        , serverMsg->toString().c_str());
                             Logger::getLogger()->info("[ChronoLog Client] Latency: {} ms", duration.count() * 1000);

                             usleep(3e6);
                             ClientMessage clientMsg;
                             clientMsg.msgType_ = ClientMessage::GETTIMEINFO;
                             std::unique_ptr <std::ostringstream> oss;
                             {
                                 oss = pSerDe->serializeClientMessage(clientMsg);
                             }
                             Logger::getLogger()->debug("[ChronoLog Client] Serialized client msg (len: {}): {}"
                                                        , oss->str().length(), oss->str().c_str());
                             t1 = std::chrono::high_resolution_clock::now();
                             client.send(oss->str());
                         });

    return client.loop();
}