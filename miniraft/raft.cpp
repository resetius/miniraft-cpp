#include <chrono>
#include <climits>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <iterator>

#include <string.h>
#include <assert.h>

#include "raft.h"
#include "messages.h"
#include "timesource.h"

namespace {

static uint32_t rand_(uint32_t* seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

} // namespace

void TDummyRsm::Validate(TMessageHolder<TCommandRequest> message) const {
    if (!message || message->Len < sizeof(TCommandRequest) ||
        (!(message->Flags & TCommandRequest::EWrite) && message->Len < sizeof(TCommandRequest) + sizeof(int64_t)))
        throw TMessageError("Invalid dummy RSM request");
}

TMessageHolder<TMessage> TDummyRsm::Read(TMessageHolder<TCommandRequest> message, uint64_t index)
{
    if (!message || message->Len < sizeof(TCommandRequest) + sizeof(int64_t))
        throw TMessageError("Missing log read index");
    int64_t readIndex;
    memcpy(&readIndex, message->Data, sizeof(readIndex));
    if (readIndex > 0 && readIndex <= Log.size()) {
        auto log = Log[readIndex-1];
        auto size = log->Len - sizeof(TLogEntry);
        auto mes = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse) + size);
        mes->Index = index;
        memcpy(mes->Data, log->Data, size);
        return mes;
    } else {
        return NewHoldedMessage<TCommandResponse>(TCommandResponse {.Index = index});
    }
}

TMessageHolder<TMessage> TDummyRsm::Write(TMessageHolder<TLogEntry> message, uint64_t index)
{
    if (LastAppliedIndex < index) {
        Log.emplace_back(std::move(message));
        LastAppliedIndex = index;
    }
    return {};
}

TMessageHolder<TLogEntry> TDummyRsm::Prepare(TMessageHolder<TCommandRequest> command)
{
    if (!command || command->Len < sizeof(TCommandRequest)) throw TMessageError("Short command");
    auto dataSize = command->Len - sizeof(TCommandRequest);
    auto entry = NewHoldedMessage<TLogEntry>(sizeof(TLogEntry)+dataSize);
    memcpy(entry->Data, command->Data, dataSize);
    return entry;
}

TMessageHolder<TInstallSnapshotRequest> TDummyRsm::Snapshot() const
{
    throw std::runtime_error("Not implemented");
}

void TDummyRsm::Apply(TMessageHolder<TInstallSnapshotRequest> snapshot)
{
    throw std::runtime_error("Not implemented");
}

TVolatileState& TVolatileState::SetElectionDue(ITimeSource::Time due) {
    ElectionDue = due;
    return *this;
}

TVolatileState& TVolatileState::Vote(uint32_t nodeId)
{
    Votes.insert(nodeId);
    return *this;
}

TVolatileState& TVolatileState::CommitAdvance(int nservers, const IState& state)
{
    auto lastIndex = state.LastLogIndex;
    Indices.clear(); Indices.reserve(nservers);
    for (auto [_, index] : MatchIndex) {
        Indices.push_back(index);
    }
    Indices.push_back(lastIndex);
    while (Indices.size() < nservers) {
        Indices.push_back(0);
    }
    std::sort(Indices.begin(), Indices.end());
    // Ascending order: a strict majority must have replicated this index.
    // For even cluster sizes, the upper median would accept only half.
    auto commitIndex = std::max(CommitIndex, Indices[(nservers - 1) / 2]);
    if (state.LogTerm(commitIndex) == state.CurrentTerm) {
        CommitIndex = commitIndex;
    }
    return *this;
}

TVolatileState& TVolatileState::SetNextIndex(uint32_t id, uint64_t nextIndex)
{
    NextIndex[id] = nextIndex;
    return *this;
}

TVolatileState& TVolatileState::SetMatchIndex(uint32_t id, uint64_t matchIndex)
{
    MatchIndex[id] = matchIndex;
    return *this;
}

TVolatileState& TVolatileState::SetHearbeatDue(uint32_t id, ITimeSource::Time heartbeatDue)
{
    HeartbeatDue[id] = heartbeatDue;
    return *this;
}

TVolatileState& TVolatileState::SetRpcDue(uint32_t id, ITimeSource::Time rpcDue)
{
    RpcDue[id] = rpcDue;
    return *this;
}

TVolatileState& TVolatileState::SetBatchSize(uint32_t id, int size)
{
    BatchSize[id] = size;
    return *this;
}

TVolatileState& TVolatileState::SetBackOff(uint32_t id, int size) {
    BackOff[id] = size;
    return *this;
}

TVolatileState& TVolatileState::SetLeaderId(uint32_t id) {
    LeaderId = id;
    return *this;
}

TVolatileState& TVolatileState::SetCommitIndex(uint64_t index)
{
    CommitIndex = index;
    return *this;
}

TRaft::TRaft(std::shared_ptr<IState> state, int node, const TNodeDict& nodes)
    : Id(node)
    , Nodes(nodes)
    , MinVotes((nodes.size()+2+nodes.size()%2)/2)
    , Npeers(nodes.size())
    , Nservers(nodes.size()+1)
    , State(std::move(state))
    , VolatileState(std::make_unique<TVolatileState>())
    , StateName(EState::FOLLOWER)
{
    if (node <= 0 || !State) throw std::invalid_argument("Raft requires a positive ID and state");
    for (const auto& [id, peer] : Nodes) {
        if (id == 0 || id == Id || !peer)
            throw std::invalid_argument("Invalid Raft peer configuration");
        VolatileState->NextIndex[id] = 1;
    }
    // A snapshot holds only committed entries.
    VolatileState->CommitIndex = State->SnapshotIndex;
}

void TRaft::OnRequestVote(ITimeSource::Time now, TMessageHolder<TRequestVoteRequest> message) {
    if (message->Term < State->CurrentTerm) {
        auto reply = NewHoldedMessage(
            TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm},
            TRequestVoteResponse {.VoteGranted = false});
        Nodes[reply->Dst]->Send(std::move(reply));
    } else if (message->Term == State->CurrentTerm) {
        bool accept = false;
        if (State->VotedFor == 0 || State->VotedFor == message->CandidateId) {
            if (message->LastLogTerm > State->LogTerm()) {
                accept = true;
            } else if (message->LastLogTerm == State->LogTerm() && message->LastLogIndex >= State->LastLogIndex) {
                accept = true;
            }
        }

        auto reply = NewHoldedMessage(
            TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm},
            TRequestVoteResponse {.VoteGranted = accept});

        if (accept) {
            VolatileState->ElectionDue = MakeElection(now);
            State->VotedFor = message->CandidateId;
            State->Commit();
        }

        Nodes[reply->Dst]->Send(std::move(reply));
    }
}

void TRaft::OnRequestVote(TMessageHolder<TRequestVoteResponse> message) {
    if (message->VoteGranted && message->Term == State->CurrentTerm) {
        (*VolatileState)
            .Vote(message->Src)
            .SetRpcDue(message->Src, ITimeSource::Max);
    }
}

void TRaft::OnAppendEntries(ITimeSource::Time now, TMessageHolder<TAppendEntriesRequest> message) {
    if (message->Term < State->CurrentTerm) {
        auto reply = NewHoldedMessage(
            TMessageEx {
                .Src = Id,
                .Dst = message->Src,
                .Term = State->CurrentTerm,
                .Seqno = message->Seqno,
            },
            TAppendEntriesResponse {
                .MatchIndex = 0,
                .Success = false,
            });
        Nodes[reply->Dst]->Send(std::move(reply));
        return;
    }

    assert(message->Term == State->CurrentTerm);

    uint64_t matchIndex = 0;
    uint64_t commitIndex = VolatileState->CommitIndex;
    bool success = false;
    // The snapshot is committed, so the leader has the same entries up to its index.
    const auto snapshotIndex = State->SnapshotIndex;
    if (message->PrevLogIndex < snapshotIndex ||
        (message->PrevLogIndex <= State->LastLogIndex
            && State->LogTerm(message->PrevLogIndex) == message->PrevLogTerm))
    {
        success = true;
        auto index = message->PrevLogIndex;
        for (uint32_t i = 0 ; i < message.PayloadSize; i++) {
            auto& data = message.Payload[i];
            auto entry = data.Cast<TLogEntry>();
            index++;
            if (index <= snapshotIndex) {
                continue;
            }
            // replace or append log entries
            if (State->LogTerm(index) != entry->Term) {
                while (State->LastLogIndex > index-1) {
                    State->RemoveLast();
                }
                State->Append(entry);
            }
        }

        matchIndex = index;
        commitIndex = std::max(commitIndex, std::min(message->LeaderCommit, matchIndex));
    }

    auto reply = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm, .Seqno = message->Seqno},
        TAppendEntriesResponse {.MatchIndex = matchIndex, .Success = success});

    (*VolatileState)
        .SetLeaderId(message->LeaderId)
        .SetCommitIndex(commitIndex)
        .SetElectionDue(MakeElection(now));
    VolatileState->LeaderSeen = now;

    Become(EState::FOLLOWER);
    Nodes[reply->Dst]->Send(std::move(reply));
}

void TRaft::OnAppendEntries(TMessageHolder<TAppendEntriesResponse> message) {
    if (message->Term != State->CurrentTerm) {
        return;
    }

    auto nodeId = message->Src;
    if (!Nodes.contains(nodeId)) return;
    auto pending = VolatileState->PendingAppendRpcs.find(nodeId);
    if (pending == VolatileState->PendingAppendRpcs.end()) return;
    auto& requests = pending->second;
    auto request = std::find_if(requests.begin(), requests.end(), [&](const auto& rpc) {
        return rpc.Seqno == message->Seqno;
    });
    if (request == requests.end() ||
        (message->Success && message->MatchIndex != request->LastIndex)) return;
    const auto rpc = *request;
    // A newer response supersedes older RPCs. Reordered/duplicate failures
    // must not undo progress or repeatedly increase backoff.
    requests.erase(requests.begin(), std::next(request));
    if (message->Success) {
        if (message->Seqno > 0 && message->Seqno <= VolatileState->LastSentSeqno[nodeId]) {
            auto& acknowledged = VolatileState->ReadAckSeqno[nodeId];
            acknowledged = std::max(acknowledged, message->Seqno);
        }
        auto matchIndex = std::max(VolatileState->MatchIndex[nodeId], message->MatchIndex);
        (*VolatileState)
            .SetMatchIndex(nodeId, matchIndex)
            .SetNextIndex(nodeId, matchIndex+1)
            .SetRpcDue(nodeId, ITimeSource::Time{})
            .SetBatchSize(nodeId, 1024)
            .SetBackOff(nodeId, 1)
            .CommitAdvance(Nservers, *State);
    } else {
        if (rpc.PrevIndex <= VolatileState->MatchIndex[nodeId]) {
            // The follower lost an entry it acknowledged in this term, so it
            // restarted without stable storage: forget its progress to let the
            // backoff reach its log. CommitIndex never moves back.
            VolatileState->MatchIndex[nodeId] = 0;
        }
        auto backOff = std::max(VolatileState->BackOff[nodeId], 1);
        auto nextIndex = VolatileState->NextIndex[nodeId] > backOff
            ? VolatileState->NextIndex[nodeId] - backOff
            : 0;
        (*VolatileState)
            .SetNextIndex(nodeId, std::max(VolatileState->MatchIndex[nodeId] + 1, nextIndex))
            .SetRpcDue(nodeId, ITimeSource::Time{})
            .SetBatchSize(nodeId, 1)
            .SetBackOff(nodeId, std::min(32768, backOff << 1));
    }
}

void TRaft::OnInstallSnapshot(ITimeSource::Time now, TMessageHolder<TInstallSnapshotRequest> message) {
    auto reply = [&](uint64_t matchIndex, uint64_t offset) {
        Nodes[message->Src]->Send(NewHoldedMessage(
            TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm, .Seqno = message->Seqno},
            TInstallSnapshotResponse {.MatchIndex = matchIndex, .Offset = offset}));
    };
    if (message->Term < State->CurrentTerm) {
        reply(0, 0);
        return;
    }

    (*VolatileState)
        .SetLeaderId(message->LeaderId)
        .SetElectionDue(MakeElection(now));
    VolatileState->LeaderSeen = now;
    Become(EState::FOLLOWER);

    const auto index = message->LastIncludedIndex;
    const auto term = message->LastIncludedTerm;
    auto& incoming = VolatileState->Incoming;
    auto& records = VolatileState->IncomingRecords;
    if (index < State->SnapshotIndex || (index <= State->LastLogIndex && State->LogTerm(index) == term)) {
        // The log has this prefix already, it is committed with the image.
        incoming = {};
        records.clear();
        VolatileState->CommitIndex = std::max(VolatileState->CommitIndex, index);
        reply(index, 0);
        return;
    }

    const bool same = incoming && VolatileState->IncomingTerm == message->Term &&
        incoming->LastIncludedIndex == index && incoming->LastIncludedTerm == term;
    if (!same) {
        // Another image starts with its first chunk; the leader restarts from it.
        if (message->Offset) {
            reply(0, 0);
            return;
        }
        incoming = message;
        VolatileState->IncomingTerm = message->Term;
        records.clear();
    }
    if (message->Offset <= records.size()) {
        for (auto i = records.size() - message->Offset; i < message.PayloadSize; ++i) {
            records.push_back(message.Payload[i]);
        }
    }
    if (!message->Done || records.size() != message->Offset + message.PayloadSize) {
        reply(0, records.size());
        return;
    }

    // Nothing changes before the last chunk.
    auto snapshot = NewHoldedMessage(TMessageEx {}, TInstallSnapshotRequest {
        .LastIncludedIndex = index,
        .LastIncludedTerm = term,
        .Nentries = uint32_t(records.size()),
        .Done = 1,
    });
    if (!records.empty()) {
        snapshot.InitPayload(records.size());
        std::copy(records.begin(), records.end(), snapshot.Payload.get());
    }
    try {
        State->InstallSnapshot(snapshot);
    } catch (...) {
        // The failure goes to the embedder. The leader resends the last chunk:
        // then the kept image is installed again, or acknowledged if the storage has it.
        if (State->SnapshotIndex >= index) {
            incoming = {};
            records.clear();
            VolatileState->CommitIndex = std::max(VolatileState->CommitIndex, State->SnapshotIndex);
        }
        throw;
    }
    incoming = {};
    records.clear();
    VolatileState->CommitIndex = std::max(VolatileState->CommitIndex, index);
    reply(index, 0);
}

void TRaft::OnInstallSnapshot(TMessageHolder<TInstallSnapshotResponse> message) {
    if (message->Term != State->CurrentTerm) {
        return;
    }

    const auto nodeId = message->Src;
    auto transfer = VolatileState->SnapshotTransfers.find(nodeId);
    if (transfer == VolatileState->SnapshotTransfers.end() || !transfer->second.Seqno ||
        transfer->second.Seqno != message->Seqno) {
        return;
    }
    const auto index = transfer->second.Snapshot->LastIncludedIndex;
    if (message->MatchIndex && message->MatchIndex != index) {
        return;
    }
    transfer->second.Seqno = 0;
    VolatileState->RpcDue[nodeId] = ITimeSource::Time{};
    if (message->MatchIndex) {
        VolatileState->SnapshotTransfers.erase(transfer);
        // Not a read acknowledgement: AppendEntries confirms reads.
        auto matchIndex = std::max(VolatileState->MatchIndex[nodeId], index);
        (*VolatileState)
            .SetMatchIndex(nodeId, matchIndex)
            .SetNextIndex(nodeId, matchIndex+1)
            .SetBatchSize(nodeId, 1024)
            .SetBackOff(nodeId, 1)
            .CommitAdvance(Nservers, *State);
    } else {
        // The follower cannot hold records it was not sent.
        transfer->second.Offset = std::min(message->Offset, transfer->second.End);
    }
}

TMessageHolder<TRequestVoteRequest> TRaft::CreateVote(uint32_t nodeId) {
    auto mes = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = nodeId, .Term = State->CurrentTerm},
        TRequestVoteRequest {
            .LastLogIndex = State->LastLogIndex,
            .LastLogTerm = State->LastLogTerm,
            .CandidateId = Id,
        });
    return mes;
}

TMessageHolder<TAppendEntriesRequest> TRaft::CreateAppendEntries(uint32_t nodeId) {
    int batchSize = std::max(1, VolatileState->BatchSize[nodeId]);
    auto prevIndex = VolatileState->NextIndex[nodeId] - 1;
    auto lastIndex = std::min<uint64_t>(prevIndex+batchSize, State->LastLogIndex);
    if (VolatileState->MatchIndex[nodeId]+1 < VolatileState->NextIndex[nodeId]) {
        lastIndex = prevIndex;
    }
    if (prevIndex < State->SnapshotIndex) {
        // The entries before the snapshot are gone: probe at its boundary.
        prevIndex = lastIndex = State->SnapshotIndex;
    }

    const TMessageLimits limits;
    uint64_t bytes = sizeof(TAppendEntriesRequest);
    std::vector<TMessageHolder<TLogEntry>> entries;
    for (auto i = prevIndex + 1; i <= lastIndex && entries.size() < limits.MaxEntries; ++i) {
        auto entry = State->Get(i);
        ValidateMessageHeader(entry->Type, entry->Len, limits);
        if (entry->Len > limits.MaxBatchBytes - bytes) break;
        bytes += entry->Len;
        entries.push_back(std::move(entry));
    }
    lastIndex = prevIndex + entries.size();

    auto mes = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = nodeId, .Term = State->CurrentTerm, .Seqno = NextSeqno()},
        TAppendEntriesRequest {
            .PrevLogIndex = prevIndex,
            .PrevLogTerm = State->LogTerm(prevIndex),
            .LeaderCommit = std::min(VolatileState->CommitIndex, lastIndex),
            .LeaderId = Id,
            .Nentries = static_cast<uint32_t>(lastIndex - prevIndex),
        });

    if (!entries.empty()) {
        mes.InitPayload(entries.size());
        for (uint32_t i = 0; i < entries.size(); ++i) mes.Payload[i] = entries[i];
    }
    VolatileState->LastSentSeqno[nodeId] = mes->Seqno;
    auto& pending = VolatileState->PendingAppendRpcs[nodeId];
    pending.push_back({mes->Seqno, prevIndex, lastIndex});
    if (pending.size() > TVolatileState::MaxPendingAppendRpcs) pending.pop_front();
    return mes;
}

TMessageHolder<TInstallSnapshotRequest> TRaft::CreateInstallSnapshot(uint32_t nodeId) {
    auto& transfer = VolatileState->SnapshotTransfers[nodeId];
    // A newer compaction does not restart a transfer, or it might never end.
    if (!transfer.Snapshot || transfer.Snapshot->LastIncludedIndex < VolatileState->NextIndex[nodeId]) {
        transfer = TVolatileState::TSnapshotTransfer {.Snapshot = State->GetSnapshot()};
    }
    const auto& snapshot = transfer.Snapshot;
    const TMessageLimits limits;
    uint64_t bytes = sizeof(TInstallSnapshotRequest);
    auto end = transfer.Offset;
    while (end < snapshot.PayloadSize && end - transfer.Offset < limits.MaxEntries &&
        snapshot.Payload[end]->Len <= limits.MaxBatchBytes - bytes)
    {
        bytes += snapshot.Payload[end++]->Len;
    }
    if (!transfer.Seqno) {
        transfer.Seqno = NextSeqno();
    }
    transfer.End = end;

    auto mes = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = nodeId, .Term = State->CurrentTerm, .Seqno = transfer.Seqno},
        TInstallSnapshotRequest {
            .LastIncludedIndex = snapshot->LastIncludedIndex,
            .LastIncludedTerm = snapshot->LastIncludedTerm,
            .Offset = transfer.Offset,
            .LeaderId = Id,
            .Nentries = uint32_t(end - transfer.Offset),
            .Done = end == snapshot.PayloadSize,
        });
    if (end > transfer.Offset) {
        mes.InitPayload(end - transfer.Offset);
        std::copy(snapshot.Payload.get() + transfer.Offset, snapshot.Payload.get() + end, mes.Payload.get());
    }
    return mes;
}

void TRaft::Follower(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    } else if (auto maybeInstallSnapshot = message.Maybe<TInstallSnapshotRequest>()) {
        OnInstallSnapshot(now, std::move(maybeInstallSnapshot.Cast()));
    }
}

void TRaft::Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeResponseVote = message.Maybe<TRequestVoteResponse>()) {
        OnRequestVote(std::move(maybeResponseVote.Cast()));
    } else if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    } else if (auto maybeInstallSnapshot = message.Maybe<TInstallSnapshotRequest>()) {
        OnInstallSnapshot(now, std::move(maybeInstallSnapshot.Cast()));
    }
}

void TRaft::Leader(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeAppendEntries = message.Maybe<TAppendEntriesResponse>()) {
        OnAppendEntries(std::move(maybeAppendEntries.Cast()));
    } else if (auto maybeInstallSnapshot = message.Maybe<TInstallSnapshotResponse>()) {
        OnInstallSnapshot(std::move(maybeInstallSnapshot.Cast()));
    } else if (auto maybeVoteRequest = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeVoteRequest.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    } else if (auto maybeInstallSnapshot = message.Maybe<TInstallSnapshotRequest>()) {
        OnInstallSnapshot(now, std::move(maybeInstallSnapshot.Cast()));
    }
}

void TRaft::Become(EState newStateName) {
    if (StateName != newStateName) {
        StateName = newStateName;
    }
}

void TRaft::Process(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo) {
    if (!ValidateRpc(message)) return;
    if (message.IsEx()) {
        auto messageEx = message.Cast<TMessageEx>();
        // Ongaro's thesis, 4.2.3: while the leader is heard from, a vote request does not
        // change the term, so a server that misses its heartbeats cannot depose it.
        if (messageEx->Type == uint32_t(EMessageType::REQUEST_VOTE_REQUEST) && messageEx->Term > State->CurrentTerm &&
            (StateName == EState::LEADER || now < VolatileState->LeaderSeen + TTimeout::Election)) {
            return;
        }
        if (messageEx->Term > State->CurrentTerm) {
            State->CurrentTerm = messageEx->Term;
            State->VotedFor = 0;
            State->Commit();
            StateName = EState::FOLLOWER;
            VolatileState->LeaderId = 0;
            VolatileState->SnapshotTransfers.clear();
            VolatileState->Incoming = {};
            VolatileState->IncomingRecords.clear();
            if (VolatileState->ElectionDue <= now || VolatileState->ElectionDue == ITimeSource::Max) {
                VolatileState->ElectionDue = MakeElection(now);
            }
        }
    }

    switch (StateName) {
    case EState::FOLLOWER:
        Follower(now, std::move(message));
        break;
    case EState::CANDIDATE:
        Candidate(now, std::move(message));
        break;
    case EState::LEADER:
        Leader(now, std::move(message));
        break;
    default:
        throw std::logic_error("Unknown state");
    }
}

bool TRaft::ValidateRpc(TMessageHolder<TMessage> message) const {
    if (!message || !message.IsEx()) return false;
    try { ValidateMessageTree(message); } catch (const TMessageError&) { return false; }
    auto header = message.Cast<TMessageEx>();
    if (header->Dst != Id || !Nodes.contains(header->Src)) return false;
    constexpr uint64_t maxIndex = INT64_MAX;
    switch (EMessageType(message->Type)) {
    case EMessageType::REQUEST_VOTE_REQUEST: {
        auto vote = message.Cast<TRequestVoteRequest>();
        return vote->CandidateId == header->Src && vote->LastLogIndex <= maxIndex &&
            vote->LastLogTerm <= header->Term &&
            ((vote->LastLogIndex == 0) == (vote->LastLogTerm == 0));
    }
    case EMessageType::REQUEST_VOTE_RESPONSE:
        return message.Cast<TRequestVoteResponse>()->VoteGranted <= 1;
    case EMessageType::APPEND_ENTRIES_RESPONSE: {
        auto reply = message.Cast<TAppendEntriesResponse>();
        return reply->Success <= 1 && reply->MatchIndex <= maxIndex &&
            (reply->Success ? reply->MatchIndex <= State->LastLogIndex : reply->MatchIndex == 0);
    }
    case EMessageType::INSTALL_SNAPSHOT_RESPONSE: {
        auto reply = message.Cast<TInstallSnapshotResponse>();
        return reply->MatchIndex <= State->LastLogIndex;
    }
    case EMessageType::INSTALL_SNAPSHOT_REQUEST: {
        auto snapshot = message.Cast<TInstallSnapshotRequest>();
        const auto index = snapshot->LastIncludedIndex;
        const auto term = snapshot->LastIncludedTerm;
        if (snapshot->LeaderId != header->Src || !index || index > maxIndex || !term || term > header->Term ||
            snapshot->Done > 1 || snapshot->Offset > UINT32_MAX - snapshot->Nentries) {
            return false;
        }
        // An image never contradicts the committed prefix.
        const auto commitIndex = VolatileState->CommitIndex;
        if (index > commitIndex) {
            return term >= State->LogTerm(commitIndex);
        }
        return index < State->SnapshotIndex ? term <= State->SnapshotTerm : State->LogTerm(index) == term;
    }
    case EMessageType::APPEND_ENTRIES_REQUEST: {
        auto append = message.Cast<TAppendEntriesRequest>();
        if (append->LeaderId != header->Src || append->PrevLogIndex > maxIndex ||
            append->LeaderCommit > maxIndex || append->Nentries > maxIndex - append->PrevLogIndex ||
            append->PrevLogTerm > header->Term ||
            ((append->PrevLogIndex == 0) != (append->PrevLogTerm == 0))) return false;
        auto previousTerm = append->PrevLogTerm;
        for (uint32_t i = 0; i < message.PayloadSize; ++i) {
            auto entry = message.Payload[i].Cast<TLogEntry>();
            if (!entry->Term || entry->Term < previousTerm || entry->Term > header->Term) return false;
            previousTerm = entry->Term;
            const auto index = append->PrevLogIndex + i + 1;
            if (index <= VolatileState->CommitIndex && index >= State->SnapshotIndex &&
                State->LogTerm(index) != entry->Term) return false;
        }
        return true;
    }
    default: return false;
    }
}

void TRaft::FollowerTimeout(ITimeSource::Time now) {
    if (VolatileState->ElectionDue <= now) {
        Become(EState::CANDIDATE);
    }
}

void TRaft::CandidateTimeout(ITimeSource::Time now) {
    for (auto& [id, node] : Nodes) {
        if (VolatileState->RpcDue[id] <= now) {
            VolatileState->RpcDue[id] = now + TTimeout::Rpc;
            node->Send(CreateVote(id));
        }
    }
}

void TRaft::LeaderTimeout(ITimeSource::Time now) {
    for (auto& [id, node] : Nodes) {
        const auto nextIndex = VolatileState->NextIndex[id];
        const bool install = nextIndex <= State->SnapshotIndex;
        const bool behind = nextIndex <= State->LastLogIndex && (install || nextIndex > State->SnapshotIndex);
        if (VolatileState->HeartbeatDue[id] <= now
            || (behind && VolatileState->RpcDue[id] <= now))
        {
            VolatileState->HeartbeatDue[id] = now + TTimeout::Election / 2;
            VolatileState->RpcDue[id] = now + TTimeout::Rpc;
            if (install) {
                node->Send(CreateInstallSnapshot(id));
            } else {
                node->Send(CreateAppendEntries(id));
            }
        }
    }

    if (Nservers == 1) {
        VolatileState->CommitAdvance(Nservers, *State);
    }
}

uint64_t TRaft::ApproveRead() {
    if (StateName != EState::LEADER) throw std::logic_error("Only a leader can approve a read");
    // Reserve a unique barrier even for a single-node cluster. Every request
    // emitted below is newer than this barrier; old replies cannot satisfy it.
    const uint64_t seqno = NextSeqno();
    for (auto& [id, node] : Nodes) {
        // A peer behind the snapshot cannot confirm a read.
        if (VolatileState->NextIndex[id] > State->SnapshotIndex) {
            node->Send(CreateAppendEntries(id));
        }
    }
    return seqno;
}

uint64_t TRaft::CommitSeqno() const {
    if (StateName != EState::LEADER || VolatileState->CommitIndex == 0 ||
        State->LogTerm(VolatileState->CommitIndex) != State->CurrentTerm) return 0;
    std::vector<uint64_t> acknowledgements{Seqno - 1}; // leader's own confirmation
    acknowledgements.reserve(Nservers);
    for (const auto& [id, _] : Nodes) {
        auto it = VolatileState->ReadAckSeqno.find(id);
        acknowledgements.push_back(it == VolatileState->ReadAckSeqno.end() ? 0 : it->second);
    }
    std::sort(acknowledgements.begin(), acknowledgements.end());
    return acknowledgements[(Nservers - 1) / 2];
}

uint64_t TRaft::NextSeqno() {
    if (Seqno == UINT64_MAX) throw std::overflow_error("Raft RPC sequence exhausted");
    return Seqno++;
}

void TRaft::ProcessTimeout(ITimeSource::Time now) {
    if (StateName == EState::CANDIDATE || StateName == EState::FOLLOWER) {
        if (VolatileState->ElectionDue <= now) {
            if (State->CurrentTerm == UINT64_MAX) throw std::overflow_error("Raft term exhausted");
            auto nextVolatileState = std::make_unique<TVolatileState>();
            for (auto [id, _] : Nodes) {
                nextVolatileState->NextIndex[id] = 1;
            }
            nextVolatileState->ElectionDue = MakeElection(now);
            nextVolatileState->CommitIndex = VolatileState->CommitIndex;
            VolatileState = std::move(nextVolatileState);
            State->VotedFor = Id;
            State->CurrentTerm ++;
            State->Commit();
            Become(EState::CANDIDATE);
        }
    }

    if (StateName == EState::CANDIDATE) {
        int nvotes = VolatileState->Votes.size()+1;
        if (nvotes >= MinVotes) {
            auto value = State->LastLogIndex+1;
            decltype(VolatileState->NextIndex) nextIndex;
            decltype(VolatileState->RpcDue) rpcDue;
            for (auto [id, _] : Nodes) {
                nextIndex.emplace(id, value);
                rpcDue.emplace(id, ITimeSource::Max);
            }

            auto nextVolatileState = std::make_unique<TVolatileState>(TVolatileState {
                .CommitIndex = VolatileState->CommitIndex,
                .LeaderId = Id,
                .NextIndex = nextIndex,
                .RpcDue = rpcDue,
                .ElectionDue = ITimeSource::Max,
            });

            VolatileState = std::move(nextVolatileState);
            StateName = EState::LEADER;

            // Add fake empty entry for faster commit
            //
            {
                auto empty = NewHoldedMessage<TLogEntry>();
                empty->Term = State->CurrentTerm;
                empty->Flags = TLogEntry::EStub;
                State->Append(std::move(empty));
            }
        }
    }

    switch (StateName) {
    case EState::FOLLOWER:
        FollowerTimeout(now); break;
    case EState::CANDIDATE:
        CandidateTimeout(now); break;
    case EState::LEADER:
        LeaderTimeout(now); break;
    default:
        throw std::logic_error("Unknown state");
    }
}

ITimeSource::Time TRaft::MakeElection(ITimeSource::Time now) {
    uint64_t delta = (uint64_t)((1.0 + (double)rand_(&Seed) / (double)UINT_MAX) * TTimeout::Election.count());
    return now + std::chrono::milliseconds(delta);
}

uint64_t TRaft::Append(TMessageHolder<TLogEntry> entry) {
    if (StateName != EState::LEADER) throw std::logic_error("Only a leader can propose log entries");
    if (State->LastLogIndex >= uint64_t(INT64_MAX)) throw std::overflow_error("Raft log index exhausted");
    if (!entry || entry->Type != uint32_t(EMessageType::LOG_ENTRY))
        throw TMessageError("Invalid proposed log entry");
    ValidateMessageHeader(entry->Type, entry->Len);
    entry->Term = State->CurrentTerm;
    State->Append(std::move(entry));
    return State->LastLogIndex;
}

void TRaft::Compact(TMessageHolder<TInstallSnapshotRequest> snapshot) {
    if (!snapshot || (snapshot.PayloadSize && !snapshot.Payload)) {
        throw std::invalid_argument("Invalid snapshot image");
    }
    const auto index = snapshot->LastIncludedIndex;
    if (index <= State->SnapshotIndex) {
        return;
    }
    if (index > VolatileState->CommitIndex) {
        throw std::invalid_argument("Snapshot includes uncommitted entries");
    }
    for (uint32_t i = 0; i < snapshot.PayloadSize; ++i) {
        // Every record must fit in an InstallSnapshot message.
        const auto& record = snapshot.Payload[i];
        if (!record || record->Type != uint32_t(EMessageType::LOG_ENTRY)) {
            throw TMessageError("Snapshot records must be log entries");
        }
        ValidateMessageHeader(record->Type, record->Len);
    }
    auto image = NewHoldedMessage(TMessageEx{}, TInstallSnapshotRequest{
        .LastIncludedIndex = index,
        .LastIncludedTerm = State->LogTerm(index),
        .Nentries = snapshot.PayloadSize,
        .Done = 1,
    });
    image.PayloadSize = snapshot.PayloadSize;
    image.Payload = snapshot.Payload;
    State->InstallSnapshot(std::move(image));
}

uint32_t TRaft::GetLeaderId() const {
    return VolatileState->LeaderId;
}

uint64_t TRaft::GetLastIndex() const {
    return State->LastLogIndex;
}

void TRaft::RestoreAppliedIndex(uint64_t index) {
    if (index > uint64_t(INT64_MAX) || index > State->LastLogIndex)
        throw std::invalid_argument("Application progress exceeds recovered Raft log");
    // A correctly recovered application marker is evidence of commitment.
    // Protect its prefix immediately, before any AppendEntries can be handled.
    VolatileState->CommitIndex = std::max(VolatileState->CommitIndex, index);
}

TRequestProcessor::TRequestProcessor(std::shared_ptr<TRaft> raft, std::shared_ptr<IRsm> rsm, const TNodeDict& nodes)
    : Raft(std::move(raft)), Rsm(std::move(rsm)), Nodes(nodes)
{
    if (!Raft || !Rsm) throw std::invalid_argument("Request processor requires Raft and application state");
    Raft->RestoreAppliedIndex(Rsm->LastAppliedIndex);
}

void TRequestProcessor::CheckStateChange() {
    PruneRequests();
    if (WaitingStateChange.empty()) {
        return;
    }

    auto stateName = Raft->CurrentStateName();
    auto leaderId = Raft->GetLeaderId();

    if (stateName == EState::CANDIDATE || (stateName != EState::LEADER &&
        (leaderId == 0 || leaderId == Raft->GetId() || !Nodes.contains(leaderId)))) {
        return;
    }

    std::queue<TWaiting> apply;
    WaitingStateChange.swap(apply);
    while (!apply.empty()) {
        auto w = std::move(apply.front()); apply.pop();
        OnCommandRequest(std::move(w.Command), w.ReplyTo);
    }
}

void TRequestProcessor::Forward(TMessageHolder<TCommandRequest> command, const std::shared_ptr<INode>& replyTo)
{
    if (!replyTo) {
        return;
    }

    if (command->Flags & TCommandRequest::EForwarded) {
        // already forwarded
        replyTo->Send(NewHoldedMessage(TCommandResponse{.Cookie = command->Cookie, .ErrorCode = 1}));
        return;
    }

    auto stateName = Raft->CurrentStateName();
    auto leaderId = Raft->GetLeaderId();
    if (stateName == EState::CANDIDATE || leaderId == 0 ||
        leaderId == Raft->GetId() || !Nodes.contains(leaderId)) {
        WaitingStateChange.emplace(TWaiting{0, 0, std::move(command), replyTo});
        return;
    }

    if (stateName == EState::FOLLOWER) {
        if (NextForwardCookie > UINT32_MAX) {
            replyTo->Send(NewHoldedMessage(TCommandResponse{.Cookie = command->Cookie, .ErrorCode = 1}));
            return;
        }
        const auto cookie = uint32_t(NextForwardCookie++);
        auto forwarded = NewHoldedMessage<TCommandRequest>(command->Len);
        memcpy(forwarded.Mes, command.Mes, command->Len);
        forwarded->Cookie = cookie;
        forwarded->Flags |= TCommandRequest::EForwarded;
        auto peer = Nodes.at(leaderId);
        Forwarded.emplace(cookie, TForwarded{replyTo, peer, command->Cookie,
                                            leaderId, Raft->GetState()->CurrentTerm});
        peer->Send(std::move(forwarded));
        return;
    }

    assert(false && "Wrong state");
}

void TRequestProcessor::OnReadRequest(TMessageHolder<TCommandRequest> command, const std::shared_ptr<INode>& replyTo)
{
    auto stateName = Raft->CurrentStateName();
    auto flags = command->Flags;
    assert(!(flags & TCommandRequest::EWrite));

    // stale read, default read (from leader)
    if (!(flags & TCommandRequest::EConsistent) &&
        ((flags & TCommandRequest::EStale) || stateName == EState::LEADER)) {
        const bool stale = flags & TCommandRequest::EStale;
        const auto index = stale ? Raft->GetVolatileState()->CommitIndex : Raft->GetLastIndex();
        const auto term = stale ? 0 : Raft->GetState()->CurrentTerm;
        Waiting.emplace(TWaiting{index, 0, std::move(command), replyTo, term});
        return;
    }

    if (stateName != EState::LEADER) {
        return Forward(std::move(command), replyTo);
    }

    // Consistent read
    const auto term = Raft->GetState()->CurrentTerm;
    const auto index = Raft->GetLastIndex();
    auto seqno = Raft->ApproveRead();
    assert(StrongWaiting.empty() || (StrongWaiting.back().Index <= index && StrongWaiting.back().Seqno <= seqno));
    StrongWaiting.emplace(TWaiting{index, seqno, std::move(command), replyTo, term});
}

void TRequestProcessor::PruneRequests(const std::shared_ptr<INode>& disconnected) {
    const auto term = Raft->GetState()->CurrentTerm;
    const auto state = Raft->CurrentStateName();
    const bool leader = state == EState::LEADER;
    const auto leaderId = Raft->GetLeaderId();
    if (!disconnected) {
        if (RequestsTerm == term && RequestsState == state && RequestsLeaderId == leaderId) return;
        RequestsTerm = term;
        RequestsState = state;
        RequestsLeaderId = leaderId;
    }
    for (auto* queue : {&Waiting, &StrongWaiting, &WaitingStateChange}) {
        std::queue<TWaiting> retained;
        while (!queue->empty()) {
            auto waiting = std::move(queue->front());
            queue->pop();
            const bool remove = disconnected ? waiting.ReplyTo == disconnected :
                waiting.Term != 0 && (!leader || waiting.Term != term);
            if (!remove) {
                retained.push(std::move(waiting));
                continue;
            }
            if (waiting.Command->Flags & TCommandRequest::EWrite) {
                PendingWriteTerms.erase(waiting.Index);
                WriteAnswers.erase(waiting.Index);
            }
            if (!disconnected && waiting.ReplyTo) waiting.ReplyTo->Send(NewHoldedMessage(
                TCommandResponse{.Cookie = waiting.Command->Cookie, .ErrorCode = 1}));
        }
        queue->swap(retained);
    }
    CancelForwarded(disconnected);
}

void TRequestProcessor::CancelForwarded(const std::shared_ptr<INode>& disconnected, bool peerFailure) {
    for (auto it = Forwarded.begin(); it != Forwarded.end();) {
        const auto& route = it->second;
        const bool remove = disconnected ?
            (peerFailure ? route.Peer == disconnected : route.Client == disconnected) :
            Raft->CurrentStateName() != EState::FOLLOWER ||
            route.Term != Raft->GetState()->CurrentTerm || route.LeaderId != Raft->GetLeaderId();
        if (!remove) { ++it; continue; }
        auto pending = std::move(it->second);
        it = Forwarded.erase(it);
        if (!disconnected || peerFailure) pending.Client->Send(NewHoldedMessage(
            TCommandResponse{.Cookie = pending.OriginalCookie, .ErrorCode = 1}));
    }
}

void TRequestProcessor::OnWriteRequest(TMessageHolder<TCommandRequest> command, const std::shared_ptr<INode>& replyTo) {
    auto stateName = Raft->CurrentStateName();
    auto flags = command->Flags;
    assert((flags & TCommandRequest::EWrite));

    if (stateName == EState::LEADER) {
        const auto term = Raft->GetState()->CurrentTerm;
        uint64_t index = Raft->Append(std::move(Rsm->Prepare(command)));
        if (replyTo) {
            PendingWriteTerms[index] = term;
            Waiting.emplace(TWaiting{index, 0, std::move(command), replyTo, term});
        }
    } else {
        Forward(std::move(command), replyTo);
    }
}

void TRequestProcessor::OnCommandRequest(TMessageHolder<TCommandRequest> command, const std::shared_ptr<INode>& replyTo) {
    Rsm->Validate(command);
    PruneRequests();
    auto flags = command->Flags;

    if (!(flags & TCommandRequest::EWrite)) {
        return OnReadRequest(std::move(command), replyTo);
    } else {
        return OnWriteRequest(std::move(command), replyTo);
    }
}

void TRequestProcessor::OnCommandResponse(TMessageHolder<TCommandResponse> command,
                                         const std::shared_ptr<INode>& peer) {
    PruneRequests();
    auto it = Forwarded.find(command->Cookie);
    if (it == Forwarded.end() || !peer || it->second.Peer != peer) return;
    auto pending = std::move(it->second);
    Forwarded.erase(it);
    auto reply = NewHoldedMessage<TCommandResponse>(command->Len);
    memcpy(reply.Mes, command.Mes, command->Len);
    reply->Cookie = pending.OriginalCookie;
    pending.Client->Send(std::move(reply));
}

void TRequestProcessor::OnPeerDisconnected(const std::shared_ptr<INode>& peer) {
    if (peer) CancelForwarded(peer, true);
}

void TRequestProcessor::CleanUp(const std::shared_ptr<INode>& replyTo) {
    if (!replyTo) return;
    PruneRequests(replyTo);
}

void TRequestProcessor::ProcessCommitted() {
    PruneRequests();
    auto& state = Raft->GetState();
    if (Rsm->LastAppliedIndex < state->SnapshotIndex) {
        // The log before the snapshot is gone: the application starts from the image.
        auto snapshot = state->GetSnapshot();
        Rsm->Apply(snapshot);
        Rsm->LastAppliedIndex = snapshot->LastIncludedIndex;
    }
    auto commitIndex = Raft->GetVolatileState()->CommitIndex;
    // Never rewind application progress, or overflow LastAppliedIndex + 1.
    if (commitIndex <= Rsm->LastAppliedIndex) return;
    for (auto i = Rsm->LastAppliedIndex+1; i <= commitIndex; i++) {
        auto entry = state->Get(i);
        if (entry->Flags == TLogEntry::EStub) {
            continue;
        }
        auto reply = Rsm->Write(entry, i);
        auto pending = PendingWriteTerms.find(i);
        if (pending != PendingWriteTerms.end() && pending->second == entry->Term) {
            WriteAnswers.emplace(i, TAnswer {
                .Term = entry->Term,
                .Reply = reply ? reply : NewHoldedMessage(TCommandResponse {.Index = i})
            });
        }
    }
    Rsm->LastAppliedIndex = commitIndex;
}

void TRequestProcessor::ProcessWaiting() {
    PruneRequests();
    auto lastApplied = Rsm->LastAppliedIndex;
    while (!Waiting.empty()) {
        if (Waiting.front().Term != 0 && (Raft->CurrentStateName() != EState::LEADER ||
            Waiting.front().Term != Raft->GetState()->CurrentTerm)) {
            PruneRequests();
            continue;
        }
        if (Waiting.front().Index > lastApplied) break;
        auto w = Waiting.front(); Waiting.pop();
        TMessageHolder<TCommandResponse> reply;
        auto cookie = w.Command->Cookie;
        if (w.Command->Flags & TCommandRequest::EWrite) {
            auto answer = WriteAnswers.find(w.Index);
            if (answer != WriteAnswers.end() && answer->second.Term == w.Term) {
                reply = answer->second.Reply.Cast<TCommandResponse>();
            } else {
                reply = NewHoldedMessage(TCommandResponse{.ErrorCode = 1});
            }
            PendingWriteTerms.erase(w.Index);
            WriteAnswers.erase(w.Index);
        } else {
            try { reply = Rsm->Read(std::move(w.Command), w.Index).Cast<TCommandResponse>(); }
            catch (const std::exception&) {
                reply = NewHoldedMessage(TCommandResponse{.Index = w.Index, .ErrorCode = 2});
            }
        }
        reply->Cookie = cookie;
        w.ReplyTo->Send(std::move(reply));
    }

    while (!StrongWaiting.empty()) {
        if (Raft->CurrentStateName() != EState::LEADER ||
            StrongWaiting.front().Term != Raft->GetState()->CurrentTerm) {
            PruneRequests();
            continue;
        }
        if (StrongWaiting.front().Index > lastApplied ||
            StrongWaiting.front().Seqno > Raft->CommitSeqno()) break;
        auto w = StrongWaiting.front(); StrongWaiting.pop();
        TMessageHolder<TCommandResponse> reply;
        assert (!(w.Command->Flags & TCommandRequest::EWrite));
        auto cookie = w.Command->Cookie;
        try { reply = Rsm->Read(std::move(w.Command), w.Index).Cast<TCommandResponse>(); }
        catch (const std::exception&) {
            reply = NewHoldedMessage(TCommandResponse{.Index = w.Index, .ErrorCode = 2});
        }
        reply->Cookie = cookie;
        w.ReplyTo->Send(std::move(reply));
    }
}

