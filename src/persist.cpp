#include "persist.h"


TDiskState::TDiskState(const std::string& name, uint32_t id)
    : Entries(Open(name + ".entries." + std::to_string(id)))
    , Index(Open(name + ".index." + std::to_string(id)))
    , State(Open(name + ".state." + std::to_string(id)))
{
    // TODO: read all an validate
    State.read((char*)&LastLogIndex, sizeof(LastLogIndex));
    State.read((char*)&CurrentTerm, sizeof(CurrentTerm));
    State.read((char*)&VotedFor, sizeof(VotedFor));
}

std::fstream TDiskState::Open(const std::string& name)
{
    auto flags = std::ios::in | std::ios::out | std::ios::binary;
    std::fstream f(name, flags);
    if (!f.is_open()) {
        std::ofstream tmp(name, flags);
        tmp.close();
        f.open(name, flags);
    }

    if (!f.is_open()) {
        throw std::runtime_error("Cannot open file: " + name);
    }

    return f;
}

TMessageHolder<TMessage> TDiskState::Read() const
{
    decltype(TMessage::Type) type;
    decltype(TMessage::Len) len;
    if (!Entries.read((char*)&type, sizeof(type))) {
        throw std::runtime_error("Error on read 1");
    }
    if (!Entries.read((char*)&len, sizeof(len))) {
        throw std::runtime_error("Error on read 2");
    }
    auto mes = NewHoldedMessage<TMessage>(type, len);
    if (!Entries.read((char*)mes->Value, len - sizeof(TMessage))) {
        throw std::runtime_error("Error on read 3");
    }
    auto maybeAppendEntries = mes.Maybe<TAppendEntriesRequest>();
    if (maybeAppendEntries) {
        auto appendEntries = maybeAppendEntries.Cast();
        auto nentries = appendEntries->Nentries;
        mes.InitPayload(nentries);
        for (uint32_t i = 0; i < nentries; i++) {
            mes.Payload[i] = Read();
        }
    }
    return mes;
}

void TDiskState::Write(TMessageHolder<TMessage> message)
{
    Entries.write((char*)message.Mes, message->Len);
    auto payload = std::move(message.Payload);
    for (uint32_t i = 0; i < message.PayloadSize; ++i) {
        Write(std::move(payload[i]));
    }
}

void TDiskState::RemoveLast()
{
    if (LastLogIndex > 0) {
        LastLogIndex--;
        State.seekg(0);
        State.write((char*)&LastLogIndex, sizeof(LastLogIndex));
    }
}

void TDiskState::Append(TMessageHolder<TLogEntry> entry)
{
    uint64_t offset = 0;
    if (Get(LastLogIndex-1)) { // TODO: optimize
        offset = Entries.tellg();
    }

    Write(entry);
    Index.seekg(LastLogIndex * sizeof(offset));
    Index.write((char*)&LastLogIndex, sizeof(LastLogIndex));
    LastLogIndex ++;
    State.seekg(0);
    State.write((char*)&LastLogIndex, sizeof(LastLogIndex));
}

TMessageHolder<TLogEntry> TDiskState::Get(int64_t index) const
{
    if (index >= LastLogIndex || index < 0) {
        return {};
    }

    uint64_t offset = 0;
    Index.seekg(index * sizeof(offset));
    if (!Index.read((char*)&offset, sizeof(offset))) {
        return {};
    }

    Entries.seekg(offset);
    return Read().Cast<TLogEntry>();
}

void TDiskState::Commit()
{
    State.seekg(sizeof(LastLogIndex));
    State.write((char*)&CurrentTerm, sizeof(CurrentTerm));
    State.write((char*)&VotedFor, sizeof(VotedFor));
}

