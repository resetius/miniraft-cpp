#include <coroutine>
#include <exception>
#include <iostream>
#include "server.h"

TPromise<void>::TTask TWriter::Write(TMessageHolder<TMessage> message) {
    auto payload = std::move(message.Payload);
    char* p = (char*)message.Mes; // TODO: const char
    uint32_t len = message->Len;
    while (len != 0) {
        auto written = co_await Socket.WriteSome(p, len);
        p += written;
        len -= written;
    }

    for (auto&& m : payload) {
        co_await TWriter(Socket).Write(std::move(m));
    }

    co_return;
}

NNet::TSimpleTask TRaftServer::InboundCounnection(NNet::TSocket socket) {
    try {

    } catch (const std::exception & ex) {
        std::cerr << "Exception: " << ex.what() << "\n";
    }
    co_return;
}

NNet::TSimpleTask TRaftServer::Serve() {
    Socket.Bind();
    Socket.Listen();
    while (true) {
        auto client = co_await Socket.Accept();
        InboundCounnection(std::move(client));
    }
    co_return;
}
