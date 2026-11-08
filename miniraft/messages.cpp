#include "messages.h"

void ValidateMessageLimits(const TMessageLimits& limits) {
    if (limits.MaxFrameBytes < sizeof(TMessage) || limits.MaxBatchBytes < sizeof(TMessage))
        throw std::invalid_argument("Message byte limits cannot hold a header");
}

void ValidateMessageHeader(uint32_t type, uint32_t length, const TMessageLimits& limits) {
    uint32_t minimum;
    bool fixed = true;
    switch (EMessageType(type)) {
    case EMessageType::LOG_ENTRY: minimum = sizeof(TLogEntry); fixed = false; break;
    case EMessageType::REQUEST_VOTE_REQUEST: minimum = sizeof(TRequestVoteRequest); break;
    case EMessageType::REQUEST_VOTE_RESPONSE: minimum = sizeof(TRequestVoteResponse); break;
    case EMessageType::APPEND_ENTRIES_REQUEST: minimum = sizeof(TAppendEntriesRequest); break;
    case EMessageType::APPEND_ENTRIES_RESPONSE: minimum = sizeof(TAppendEntriesResponse); break;
    case EMessageType::COMMAND_REQUEST: minimum = sizeof(TCommandRequest); fixed = false; break;
    case EMessageType::COMMAND_RESPONSE: minimum = sizeof(TCommandResponse); fixed = false; break;
    default: throw TMessageError("Unknown or unsupported message type");
    }
    if (length < minimum || (fixed && length != minimum) || length > limits.MaxFrameBytes)
        throw TMessageError("Invalid message length");
}

void ValidateMessageTree(const TMessageHolder<TMessage>& message, const TMessageLimits& limits) {
    ValidateMessageLimits(limits);
    uint64_t remaining = limits.MaxBatchBytes;
    auto validate = [&](const auto& frame, bool child) {
        if (!frame) throw TMessageError("Missing message");
        ValidateMessageHeader(frame->Type, frame->Len, limits);
        if (frame->Len > remaining) throw TMessageError("Message batch exceeds byte limit");
        remaining -= frame->Len;
        if (child && frame->Type != uint32_t(EMessageType::LOG_ENTRY))
            throw TMessageError("AppendEntries children must be log entries");
        if (frame->Type != uint32_t(EMessageType::APPEND_ENTRIES_REQUEST) && frame.PayloadSize)
            throw TMessageError("Unexpected message payload");
    };
    validate(message, false);
    if (message->Type != uint32_t(EMessageType::APPEND_ENTRIES_REQUEST)) return;
    auto copy = message;
    const auto count = copy.Cast<TAppendEntriesRequest>()->Nentries;
    if (count > limits.MaxEntries || count != message.PayloadSize || (count && !message.Payload))
        throw TMessageError("Invalid AppendEntries entry count");
    for (uint32_t i = 0; i < count; ++i) validate(message.Payload[i], true);
}
