#pragma once
#include <miniraft/messages.h>

// Derived message structs may contain tail padding. Bound from the actual
// payload offset, preserving the existing native-layout wire representation.
template<class T>
void ValidatePayloadRange(const TMessageHolder<T>& message, const char* data, uint64_t size) {
    const auto offset = size_t(data - reinterpret_cast<const char*>(message.Mes));
    if (offset > message->Len || size > message->Len - offset)
        throw TMessageError("Application payload length exceeds message");
}
