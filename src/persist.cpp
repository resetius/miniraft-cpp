#include "persist.h"
#include <iostream>

TDiskState::TDiskState(const std::string& name, uint32_t id)
    : Entries(Open(name + ".entries." + std::to_string(id)))
    , Index(Open(name + ".index." + std::to_string(id)))
    , State(Open(name + ".state." + std::to_string(id)))
{
    // TODO: read all an validate
    State.seekg(0, std::ios_base::end);
    auto size = State.tellg();
    if (size == sizeof(LastLogIndex) + sizeof(CurrentTerm) + sizeof(VotedFor)) {
        State.seekg(0);
        State.read((char*)&LastLogIndex, sizeof(LastLogIndex));
        State.read((char*)&CurrentTerm, sizeof(CurrentTerm));
        State.read((char*)&VotedFor, sizeof(VotedFor));
    } else {
        Commit();
    }
    if (LastLogIndex > 0) {
        LastLogTerm = Get(LastLogIndex-1)->Term;
    }
}

std::fstream TDiskState::Open(const std::string& name)
{
    auto flags = std::ios::in | std::ios::out | std::ios::binary;
    std::fstream f(name, flags);
    if (!f.is_open()) {
        std::ofstream tmp(name);
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
    return mes;
}

void TDiskState::Write(TMessageHolder<TMessage> message)
{
    Entries.write((char*)message.Mes, message->Len);
}

void TDiskState::RemoveLast()
{
    if (LastLogIndex > 0) {
        LastLogIndex--;
        Commit();
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
    Index.write((char*)&offset, sizeof(offset));
    LastLogTerm = entry->Term;
    LastLogIndex ++;
    Commit();
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
    auto entry = Read().Cast<TLogEntry>();
    return entry;
}

void TDiskState::Commit()
{
    State.seekg(0);
    if (!State.write((char*)&LastLogIndex, sizeof(LastLogIndex))) { abort(); }
    if (!State.write((char*)&CurrentTerm, sizeof(CurrentTerm))) { abort(); }
    if (!State.write((char*)&VotedFor, sizeof(VotedFor))) { abort(); }
    State.flush();
    Index.flush();
    Entries.flush();
}

