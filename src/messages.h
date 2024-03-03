#pragma once
#include <chrono>
#include <memory>
#include <vector>

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <typeinfo>

enum class EMessageType : uint32_t {
    NONE = 0,
    LOG_ENTRY = 1,
    REQUEST_VOTE_REQUEST = 2,
    REQUEST_VOTE_RESPONSE = 3,
    APPEND_ENTRIES_REQUEST = 4,
    APPEND_ENTRIES_RESPONSE = 5,
    COMMAND_REQUEST = 6,
    COMMAND_RESPONSE = 7,
};

struct TMessage {
    static constexpr EMessageType MessageType = EMessageType::NONE;
    uint32_t Type;
    uint32_t Len;
    char Value[0];
};

static_assert(sizeof(TMessage) == 8);

struct TLogEntry: public TMessage {
    static constexpr EMessageType MessageType = EMessageType::LOG_ENTRY;
    uint64_t Term = 1;
    char Data[0];
};

struct TMessageEx: public TMessage {
    uint32_t Src = 0;
    uint32_t Dst = 0;
    uint64_t Term = 0;
};

static_assert(sizeof(TMessageEx) == sizeof(TMessage)+16);

struct TRequestVoteRequest: public TMessageEx {
    static constexpr EMessageType MessageType = EMessageType::REQUEST_VOTE_REQUEST;
    uint64_t LastLogIndex;
    uint64_t LastLogTerm;
    uint32_t CandidateId;
    uint32_t Padding = 0;
};

static_assert(sizeof(TRequestVoteRequest) == sizeof(TMessageEx)+24);

struct TRequestVoteResponse: public TMessageEx {
    static constexpr EMessageType MessageType = EMessageType::REQUEST_VOTE_RESPONSE;
    uint32_t VoteGranted;
    uint32_t Padding = 0;
};

static_assert(sizeof(TRequestVoteResponse) == sizeof(TMessageEx)+8);

struct TAppendEntriesRequest: public TMessageEx {
    static constexpr EMessageType MessageType = EMessageType::APPEND_ENTRIES_REQUEST;
    uint64_t PrevLogIndex = 0;
    uint64_t PrevLogTerm = 0;
    uint64_t LeaderCommit = 0;
    uint32_t LeaderId = 0;
    uint32_t Nentries = 0;
};

static_assert(sizeof(TAppendEntriesRequest) == sizeof(TMessageEx) + 32);

struct TAppendEntriesResponse: public TMessageEx {
    static constexpr EMessageType MessageType = EMessageType::APPEND_ENTRIES_RESPONSE;
    uint64_t MatchIndex;
    uint32_t Success;
    uint32_t Padding = 0;
};

static_assert(sizeof(TAppendEntriesResponse) == sizeof(TMessageEx) + 16);

struct TCommandRequest: public TMessage {
    static constexpr EMessageType MessageType = EMessageType::COMMAND_REQUEST;
    enum EFlags {
        ENone = 0,
        EWrite = 1,
    };
    uint32_t Flags = ENone;
    char Data[0];
};

static_assert(sizeof(TCommandRequest) == sizeof(TMessage) + 4);

struct TCommandResponse: public TMessage {
    static constexpr EMessageType MessageType = EMessageType::COMMAND_RESPONSE;
    uint64_t Index;
    char Data[0];
};

static_assert(sizeof(TCommandResponse) == sizeof(TMessage) + 8);

struct TTimeout {
    static constexpr std::chrono::milliseconds Election = std::chrono::milliseconds(5000);
    static constexpr std::chrono::milliseconds Heartbeat = std::chrono::milliseconds(1000);
    static constexpr std::chrono::milliseconds Rpc = std::chrono::milliseconds(10000);
};

template<typename T>
requires std::derived_from<T, TMessage>
struct TMessageHolder {
    T* Mes;
    std::shared_ptr<char[]> RawData;

    uint32_t PayloadSize;
    std::shared_ptr<TMessageHolder<TMessage>[]> Payload;

    TMessageHolder()
        : Mes(nullptr)
    { }

    template<typename U>
    requires std::derived_from<U, T>
    TMessageHolder(U* u,
        const std::shared_ptr<char[]>& rawData,
        uint32_t payloadSize = 0,
        const std::shared_ptr<TMessageHolder<TMessage>[]>& payload = {})
        : Mes(u)
        , RawData(rawData)
        , PayloadSize(payloadSize)
        , Payload(payload)
    { }

    template<typename U>
    requires std::derived_from<U, T>
    TMessageHolder(const TMessageHolder<U>& other)
        : Mes(other.Mes)
        , RawData(other.RawData)
        , PayloadSize(other.PayloadSize)
        , Payload(other.Payload)
    { }

    void InitPayload(uint32_t size) {
        PayloadSize = size;
        Payload = std::shared_ptr<TMessageHolder<TMessage>[]>(new TMessageHolder<TMessage>[size]);
    }

    T* operator->() {
        return Mes;
    }

    const T* operator->() const {
        return Mes;
    }

    operator bool() const {
        return !!Mes;
    }

    bool IsEx() const {
        return (2 <= Mes->Type && Mes->Type <= 5);
    }

    template<typename U>
    requires std::derived_from<U, T>
    TMessageHolder<U> Cast() {
        return TMessageHolder<U>(static_cast<U*>(Mes), RawData, PayloadSize, Payload);
    }

    template<typename U>
    requires std::derived_from<U, T>
    auto Maybe() {
        struct Maybe {
            TMessageHolder<TMessage> Mes;

            operator bool() const {
                return Mes;
            }

            TMessageHolder<U> Cast() {
                if (*this) {
                    return Mes.Cast<U>();
                }
                throw std::bad_cast();
            }
        };

        return Mes->Type == static_cast<uint32_t>(U::MessageType)
            ? Maybe { *this }
            : Maybe { };
    }
};

template<typename T>
requires std::derived_from<T, TMessage>
T* NewMessage(uint32_t type, uint32_t len) {
    assert(len >= sizeof(TMessage));
    char* data = new char[len];
    T* mes =  new (data) T;
    mes->Type = type;
    mes->Len = len;
    return mes;
}

template<typename T>
requires std::derived_from<T, TMessage>
TMessageHolder<T> NewHoldedMessage(uint32_t type, uint32_t len)
{
    T* mes = NewMessage<T>(type, len);
    return TMessageHolder<T>(mes, std::shared_ptr<char[]>(reinterpret_cast<char*>(mes)));
}

template<typename T>
TMessageHolder<T> NewHoldedMessage() {
    return NewHoldedMessage<T>(static_cast<uint32_t>(T::MessageType), sizeof(T));
}

template<typename T>
TMessageHolder<T> NewHoldedMessage(uint32_t size) {
    assert(size >= sizeof(T));
    return NewHoldedMessage<T>(static_cast<uint32_t>(T::MessageType), size);
}

template<typename T>
TMessageHolder<T> NewHoldedMessage(T t) {
    auto m = NewHoldedMessage<T>();
    memcpy((char*)m.Mes + sizeof(TMessage), (char*)&t + sizeof(TMessage), sizeof(T) - sizeof(TMessage));
    return m;
}

template<typename T>
requires std::derived_from<T, TMessageEx>
TMessageHolder<T> NewHoldedMessage(TMessageEx h, T t) {
    auto m = NewHoldedMessage<T>();
    memcpy((char*)m.Mes + sizeof(TMessage), (char*)&h + sizeof(TMessage), sizeof(h) - sizeof(TMessage));
    memcpy((char*)m.Mes + sizeof(TMessageEx), (char*)&t + sizeof(TMessageEx), sizeof(T) - sizeof(TMessageEx));
    return m;
}
