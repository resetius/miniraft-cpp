#include <string_view>

#include "kv.h"

struct TWriteKv: public TLogEntry {
    uint16_t KeySize;
    uint16_t ValSize;
    char Data[0];
};

struct TReadKv: public TCommandRequest {
    uint16_t KeySize;
    char Data[0];
};

TMessageHolder<TMessage> TKv::Read(TMessageHolder<TCommandRequest> message) {
    auto readKv = message.Cast<TReadKv>();
    std::string_view k(readKv->Data, readKv->KeySize);
    auto it = H.find(std::string(k));
    if (it == H.end()) {
        // TODO
    } else {
        // TODO
    }
    return {};
}

void TKv::Write(TMessageHolder<TLogEntry> message) {
    auto writeKv = message.Cast<TWriteKv>();
    std::string_view k(writeKv->Data, writeKv->KeySize);
    std::string_view v(writeKv->Data + writeKv->KeySize, writeKv->ValSize);
    H[std::string(k)] = std::string(v);
    return;
}

TMessageHolder<TLogEntry> TKv::Prepare(TMessageHolder<TCommandRequest> command, uint64_t term) {
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    entry->Term = term;
    return entry;
}

int main(int argc, char** argv) {
    return 0;
}
