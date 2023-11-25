#include <coroutine>
#include <exception>
#include <iostream>
#include <stdexcept>
#include "server.h"
#include "messages.h"

TPromise<void>::TTask TWriter::Write(TMessageHolder<TMessage> message) {
    auto payload = std::move(message.Payload);
    char* p = (char*)message.Mes; // TODO: const char
    uint32_t len = message->Len;
    while (len != 0) {
        auto written = co_await Socket.WriteSome(p, len);
        if (written == 0) {
            throw std::runtime_error("Connection closed");
        }
        p += written;
        len -= written;
    }

    for (auto&& m : payload) {
        co_await TWriter(Socket).Write(std::move(m));
    }

    co_return;
}

TPromise<TMessageHolder<TMessage>>::TTask TReader::Read() {
    decltype(TMessage::Type) type;
    decltype(TMessage::Len) len;
    auto s = co_await Socket.ReadSome((char*)&type, sizeof(type));
    assert(s == sizeof(type));
    s = co_await Socket.ReadSome((char*)&len, sizeof(len));
    assert(s == sizeof(len));
    auto mes = NewHoldedMessage<TMessage>(type, len);
    char* p = mes->Value;
    len -= sizeof(TMessage);
    while (len != 0) {
        s = co_await Socket.ReadSome(p, len);
        if (s == 0) {
            throw std::runtime_error("Connection closed");
        }
        p += s;
        len -= s;
    }
    auto maybeAppendEntries = mes.Maybe<TAppendEntriesRequest>();
    if (maybeAppendEntries) {
        auto appendEntries = maybeAppendEntries.Cast();
        mes.Payload.resize(appendEntries->Nentries);
        for (auto& m : mes.Payload) {
            m = co_await TReader(Socket).Read();
        }
    }
    co_return mes;
}

NNet::TSimpleTask TRaftServer::InboundCounnection(NNet::TSocket socket) {
    try {
        while (true) {
            auto mes = co_await TReader(socket).Read();
            Raft->Process(std::move(mes));
        }
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
