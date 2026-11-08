#include "kv.h"
#include "kv_messages.h"
#include "payload_validation.h"
#include <string_view>

void TKv::Validate(TMessageHolder<TCommandRequest> message) const {
    if (!message || message->Len < sizeof(TCommandRequest)) throw TMessageError("Short KV command");
    if (message->Flags & TCommandRequest::EWrite) {
        auto write = message.Cast<TWriteKv>();
        ValidatePayloadRange(message, write->TKvEntry::Data, uint64_t(write->KeySize) + write->ValSize);
    } else {
        auto read = message.Cast<TReadKv>();
        ValidatePayloadRange(message, read->Data, read->KeySize);
    }
}

TMessageHolder<TMessage> TKv::Read(TMessageHolder<TCommandRequest> message, uint64_t index) {
    auto readKv = message.Cast<TReadKv>();
    ValidatePayloadRange(message, readKv->Data, readKv->KeySize);
    if (readKv->KeySize == 0) {
        std::string data;
        for (auto& [k, v] : H) {
            if (k.size() + v.size() + 4 > TMessageLimits{}.MaxFrameBytes - sizeof(TCommandResponse) - data.size())
                throw TMessageError("KV list response exceeds frame limit");
            data += k + " = " + v + "\n";
        }
        auto res = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse) + data.size());
        memcpy(res->Data, data.data(), data.size());
        res->Index = index;
        return res;
    } else {
        std::string_view k(readKv->Data, readKv->KeySize);
        auto it = H.find(std::string(k));
        if (it == H.end()) {
            auto res = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse));
            res->Index = index;
            return res;
        } else {
            auto res = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse)+it->second.size());
            res->Index = index;
            memcpy(res->Data, it->second.data(), it->second.size());
            return res;
        }
    }
}

TMessageHolder<TMessage> TKv::Write(TMessageHolder<TLogEntry> message, uint64_t index) {
    if (LastAppliedIndex < index) {
        auto entry = message.Cast<TKvLogEntry>();
        ValidatePayloadRange(message, entry->TKvEntry::Data, uint64_t(entry->KeySize) + entry->ValSize);
        std::string_view k(entry->TKvEntry::Data, entry->KeySize);
        if (entry->ValSize) {
            std::string_view v(entry->TKvEntry::Data + entry->KeySize, entry->ValSize);
            H[std::string(k)] = std::string(v);
        } else {
            H.erase(std::string(k));
        }
        LastAppliedIndex = index;
    }
    return {};
}

TMessageHolder<TLogEntry> TKv::Prepare(TMessageHolder<TCommandRequest> command) {
    auto write = command.Cast<TWriteKv>();
    ValidatePayloadRange(command, write->TKvEntry::Data, uint64_t(write->KeySize) + write->ValSize);
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    return entry;
}

