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

struct TSnapshotMetadataProbe {
    bool Complete = false;
    bool Supported = false;
    size_t Need = 8;
};

bool ReadSnapshotInteger(std::string_view bytes, size_t offset, unsigned width, uint64_t& value) {
    if (offset > bytes.size() || width > bytes.size() - offset) return false;
    value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(uint8_t(bytes[offset + i])) << (8 * i);
    return true;
}

TSnapshotMetadataProbe ProbeSnapshotMetadata(std::string_view prefix, uint64_t totalBytes,
        uint64_t index, uint64_t term, const std::vector<uint32_t>& voters,
        const TSnapshot* knownSnapshot) {
    if (prefix.size() < 8) return {false, false, 8};
    const auto magic = prefix.substr(0, 8);
    const bool stable = magic == "MRFTS003";
    if (!stable && magic != "MRFTS001") return {true, false, 0};

    // Inspect only the bounded format header and voter list. Callers use this
    // before granting replication progress or accepting a higher term; payload
    // bytes and CRC are validated by the full install path when needed.
    const size_t fixedHeaderBytes = stable ? 52 : 36;
    if (prefix.size() < fixedHeaderBytes) return {false, false, fixedHeaderBytes};
    uint64_t embeddedIndex = 0, embeddedTerm = 0, voterCount = 0, dataBytes = 0;
    if (!ReadSnapshotInteger(prefix, 8, 8, embeddedIndex) ||
        !ReadSnapshotInteger(prefix, 16, 8, embeddedTerm) ||
        !ReadSnapshotInteger(prefix, 24, 4, voterCount) ||
        !ReadSnapshotInteger(prefix, 28, 8, dataBytes) ||
        embeddedIndex != index || embeddedTerm != term || voterCount != voters.size())
        return {true, false, 0};
    if (stable) {
        uint64_t configurationIndex = 0, configurationTerm = 0;
        if (!ReadSnapshotInteger(prefix, 36, 8, configurationIndex) ||
            !ReadSnapshotInteger(prefix, 44, 8, configurationTerm) || !configurationIndex ||
            configurationIndex > embeddedIndex || !configurationTerm || configurationTerm > embeddedTerm ||
            (configurationIndex == embeddedIndex && configurationTerm != embeddedTerm) ||
            (knownSnapshot && knownSnapshot->StableConfiguration &&
                (configurationIndex != knownSnapshot->ConfigurationIndex ||
                 configurationTerm != knownSnapshot->ConfigurationTerm)))
            return {true, false, 0};
    }

    const uint64_t voterBytes = voterCount * 4;
    const uint64_t headerBytes = fixedHeaderBytes + voterBytes;
    if (headerBytes + 4 > totalBytes || dataBytes != totalBytes - headerBytes - 4)
        return {true, false, 0};
    if (headerBytes > prefix.size()) return {false, false, size_t(headerBytes)};
    for (size_t i = 0; i < voters.size(); ++i) {
        uint64_t voter = 0;
        if (!ReadSnapshotInteger(prefix, fixedHeaderBytes + i * 4, 4, voter) || voter != voters[i])
            return {true, false, 0};
    }
    return {true, true, size_t(headerBytes)};
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

TVolatileState& TVolatileState::CommitAdvance(const std::vector<uint32_t>& voters, uint32_t self, const IState& state) {
    return CommitAdvance(TQuorum(voters), self, state);
}

TVolatileState& TVolatileState::CommitAdvance(const TQuorum& quorum, uint32_t self, const IState& state) {
    const auto quorumIndex = quorum.Value([&](uint32_t id) {
        auto match = MatchIndex.find(id);
        return id == self ? state.LastLogIndex : match == MatchIndex.end() ? uint64_t(0) : match->second;
    }, &Indices);
    const auto index = std::max(CommitIndex, quorumIndex);
    if (state.LogTerm(index) == state.CurrentTerm) CommitIndex = index;
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

TRaft::TRaft(std::shared_ptr<IState> state, int node, const TNodeDict& nodes, std::vector<uint32_t> learners)
    : Id(node)
    , Nodes(nodes)
    , MinVotes(0) // derived from voting IDs after role validation
    , Npeers(nodes.size())
    , Nservers(0)
    , State(std::move(state))
    , VolatileState(std::make_unique<TVolatileState>())
    , StateName(EState::FOLLOWER)
{
    if (node <= 0 || !State) throw std::invalid_argument("Raft requires a positive ID and state");
    // Equal seeds make simultaneously started voters campaign in lockstep.
    // An odd multiplier gives every positive node ID a distinct nonzero stream.
    Seed = uint32_t(node) * 0x9e3779b9u;
    for (const auto& [id, peer] : Nodes) {
        if (id == 0 || id == Id || !peer)
            throw std::invalid_argument("Invalid Raft peer configuration");
        VolatileState->NextIndex[id] = 1;
    }
    Learners = std::move(learners);
    std::sort(Learners.begin(), Learners.end());
    uint32_t previous = 0;
    for (auto id : Learners) {
        if (id <= previous || (id != Id && !Nodes.contains(id)))
            throw std::invalid_argument("Invalid or duplicate learner ID");
        previous = id;
    }
    if (!std::binary_search(Learners.begin(), Learners.end(), Id)) Voters.push_back(Id);
    for (const auto& [id, _] : Nodes)
        if (!std::binary_search(Learners.begin(), Learners.end(), id)) Voters.push_back(id);
    std::sort(Voters.begin(), Voters.end());
    ValidateFixedVoters(Voters);
    VotingQuorum.emplace(Voters);
    if (!State->CurrentTerm) throw std::invalid_argument("Stored Raft term must be positive");
    if (!IsVoter(Id) && State->VotedFor)
        throw std::invalid_argument("A learner store must not contain a vote");
    if (State->VotedFor && !IsVoter(State->VotedFor))
        throw std::invalid_argument("Stored vote does not name a configured voter");
    Nservers = Voters.size(); MinVotes = Nservers / 2 + 1;
    if (!Learners.empty() && !State->SupportsVoterBinding())
        throw std::invalid_argument("Learners require voter-binding-capable storage");
    const auto bound = State->GetBoundVoters();
    if (State->SnapshotIndex > State->LastLogIndex || State->LastLogIndex > uint64_t(INT64_MAX))
        throw std::invalid_argument("Invalid retained log bounds");
    const auto snapshot = State->GetSnapshot();
    if (snapshot) {
        ValidateSnapshot(*snapshot);
        if (snapshot->IsJoint())
            throw std::invalid_argument("Joint snapshot recovery is unsupported");
        if (snapshot->StableConfiguration && bound != Voters)
            throw std::invalid_argument("Stable configuration snapshot requires an existing voter binding");
        if (snapshot->Index != State->SnapshotIndex || snapshot->Term != State->SnapshotTerm ||
            snapshot->Term > State->CurrentTerm || snapshot->Voters != Voters ||
            State->LastLogTerm < State->SnapshotTerm || State->LastLogTerm > State->CurrentTerm ||
            (State->LastLogIndex == State->SnapshotIndex && State->LastLogTerm != State->SnapshotTerm))
            throw std::invalid_argument("Compacted state snapshot boundary mismatch");
    } else if (State->SnapshotIndex || State->SnapshotTerm) {
        throw std::invalid_argument("Compacted state is missing its snapshot");
    }
    if (State->HasUnboundRecoveredState() && !snapshot)
        throw std::invalid_argument("Recovered durable state requires voter proof before Raft startup");
    // Reject before binding voters or serving traffic. Current Raft cannot
    // interpret configuration entries, including uncommitted suffix entries.
    auto previousTerm = State->SnapshotTerm;
    for (uint64_t index = State->SnapshotIndex + 1; index <= State->LastLogIndex; ++index) {
        if (State->RetainedLogFlags(index) & TLogEntry::EConfiguration)
            throw std::invalid_argument("Configuration log recovery is unsupported");
        if (snapshot) {
            const auto term = State->RetainedLogTerm(index);
            if (!term || term < previousTerm || term > State->CurrentTerm)
                throw std::invalid_argument("Invalid compacted log term sequence");
            previousTerm = term;
        }
    }
    if (snapshot && previousTerm != State->LastLogTerm)
        throw std::invalid_argument("Compacted log last term mismatch");
    const bool fresh = State->CurrentTerm == 1 && !State->VotedFor && !State->LastLogIndex;
    if (State->SupportsVoterBinding() && (!bound.empty() || fresh || snapshot || !Learners.empty()))
        State->BindVoters(Voters);
    if (snapshot) {
        VolatileState->CommitIndex = State->SnapshotIndex;
    }
    State->MarkRaftStarted();
}

void TRaft::OnRequestVote(ITimeSource::Time now, TMessageHolder<TRequestVoteRequest> message) {
    if (!IsVoter(Id)) {
        Nodes.at(message->Src)->Send(NewHoldedMessage(
            TMessageEx{.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm}, TRequestVoteResponse{.VoteGranted = false}));
        return;
    }
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
    if (message->PrevLogIndex >= State->SnapshotIndex && (message->PrevLogIndex == 0 ||
        (message->PrevLogIndex <= State->LastLogIndex
            && State->LogTerm(message->PrevLogIndex) == message->PrevLogTerm)))
    {
        success = true;
        auto index = message->PrevLogIndex;
        for (uint32_t i = 0 ; i < message.PayloadSize; i++) {
            auto& data = message.Payload[i];
            auto entry = data.Cast<TLogEntry>();
            index++;
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

    if (!success && message->PrevLogIndex < State->SnapshotIndex) matchIndex = State->SnapshotIndex;
    auto reply = NewHoldedMessage(
        TMessageEx {.Src = Id, .Dst = message->Src, .Term = State->CurrentTerm, .Seqno = message->Seqno},
        TAppendEntriesResponse {.MatchIndex = matchIndex, .Success = success});

    (*VolatileState)
        .SetLeaderId(message->LeaderId)
        .SetCommitIndex(commitIndex)
        .SetElectionDue(MakeElection(now));

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
            .CommitAdvance(*VotingQuorum, Id, *State);
    } else if (message->MatchIndex > 0) {
        // A compacted-prefix hint is not replication evidence. Probe its boundary
        // with AppendEntries before advancing MatchIndex or confirming reads.
        (*VolatileState).SetNextIndex(nodeId, std::max(VolatileState->MatchIndex[nodeId] + 1, message->MatchIndex + 1))
            .SetRpcDue(nodeId, ITimeSource::Time{}).SetBatchSize(nodeId, 1).SetBackOff(nodeId, 1);
    } else {
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
    pending.push_back({mes->Seqno, lastIndex});
    if (pending.size() > TVolatileState::MaxPendingAppendRpcs) pending.pop_front();
    return mes;
}

void TRaft::Follower(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    } else if (auto snapshot = message.Maybe<TInstallSnapshotRequest>()) {
        OnInstallSnapshot(now, snapshot.Cast());
    }
}

void TRaft::Candidate(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto maybeResponseVote = message.Maybe<TRequestVoteResponse>()) {
        OnRequestVote(std::move(maybeResponseVote.Cast()));
    } else if (auto maybeRequestVote = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeRequestVote.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    } else if (auto snapshot = message.Maybe<TInstallSnapshotRequest>()) {
        OnInstallSnapshot(now, snapshot.Cast());
    }
}

void TRaft::Leader(ITimeSource::Time now, TMessageHolder<TMessage> message) {
    if (auto snapshot = message.Maybe<TInstallSnapshotResponse>()) {
        OnInstallSnapshot(snapshot.Cast());
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesResponse>()) {
        OnAppendEntries(std::move(maybeAppendEntries.Cast()));
    } else if (auto maybeVoteRequest = message.Maybe<TRequestVoteRequest>()) {
        OnRequestVote(now, std::move(maybeVoteRequest.Cast()));
    } else if (auto maybeAppendEntries = message.Maybe<TAppendEntriesRequest>()) {
        OnAppendEntries(now, std::move(maybeAppendEntries.Cast()));
    } else if (auto snapshot = message.Maybe<TInstallSnapshotRequest>()) {
        OnInstallSnapshot(now, snapshot.Cast());
    }
}

void TRaft::Become(EState newStateName) {
    if (!IsVoter(Id) && (newStateName == EState::CANDIDATE || newStateName == EState::LEADER))
        throw std::logic_error("A learner cannot campaign or lead");
    if (StateName != newStateName) {
        StateName = newStateName;
    }
}

void TRaft::Process(ITimeSource::Time now, TMessageHolder<TMessage> message, const std::shared_ptr<INode>& replyTo) {
    if (!ValidateRpc(message)) return;
    if (message.IsEx()) {
        auto messageEx = message.Cast<TMessageEx>();
        if (messageEx->Term > State->CurrentTerm) {
            State->CurrentTerm = messageEx->Term;
            State->VotedFor = 0;
            State->Commit();
            StateName = EState::FOLLOWER;
            IncomingSnapshot.reset();
            VolatileState->SnapshotRpcs.clear();
            EncodedSnapshot.reset();
            EncodedSnapshotIndex = 0;
            VolatileState->LeaderId = 0;
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
    if (!IsVoter(header->Src)) {
        if ((header->Type != uint32_t(EMessageType::APPEND_ENTRIES_RESPONSE) &&
             header->Type != uint32_t(EMessageType::INSTALL_SNAPSHOT_RESPONSE)) || header->Term != State->CurrentTerm)
            return false; // Nonvoters cannot campaign, lead or force a term change.
    }
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
            reply->MatchIndex <= State->LastLogIndex;
    }
    case EMessageType::INSTALL_SNAPSHOT_REQUEST: {
        auto request = message.Cast<TInstallSnapshotRequest>();
        const auto length = request->Len - sizeof(TInstallSnapshotRequest);
        // A later boundary cannot have an older term than the committed prefix.
        // For an already committed, retained boundary, the terms must match;
        // an older compacted boundary cannot have a newer term than the snapshot.
        const bool valid = request->LeaderId == header->Src && request->SnapshotIndex > 0 && request->SnapshotIndex <= maxIndex &&
            request->SnapshotTerm > 0 && request->SnapshotTerm <= header->Term && request->Done <= 1 &&
            (request->SnapshotIndex <= VolatileState->CommitIndex ||
                request->SnapshotTerm >= State->LogTerm(VolatileState->CommitIndex)) &&
            (request->SnapshotIndex > VolatileState->CommitIndex || request->SnapshotIndex < State->SnapshotIndex ||
                request->SnapshotTerm == State->LogTerm(request->SnapshotIndex)) &&
            (request->SnapshotIndex >= State->SnapshotIndex || request->SnapshotTerm <= State->SnapshotTerm) &&
            request->TotalBytes >= 44 && request->TotalBytes <= TSnapshot::MaxBytes &&
            request->Offset <= request->TotalBytes && length > 0 && length <= TInstallSnapshotRequest::MaxChunkBytes &&
            length <= request->TotalBytes - request->Offset &&
            bool(request->Done) == (request->Offset + length == request->TotalBytes);
        if (!valid) return false;
        if (header->Term > State->CurrentTerm) {
            // Do not persist a higher term based on an InstallSnapshot request
            // whose format cannot be accepted by this runtime. A continuation
            // cannot be valid across a term change because term changes discard
            // the partial transfer; an initial chunk must identify a supported
            // fixed/stable format and complete metadata prefix before it can
            // establish leader contact.
            if (request->Offset != 0) return false;
            const auto knownSnapshot = State->GetSnapshot();
            const auto probe = ProbeSnapshotMetadata(std::string_view(request->Data, length),
                request->TotalBytes, request->SnapshotIndex, request->SnapshotTerm,
                GetVoterIds(), knownSnapshot.get());
            if (!probe.Complete || !probe.Supported) return false;
        }
        return true;
    }
    case EMessageType::INSTALL_SNAPSHOT_RESPONSE: {
        auto response = message.Cast<TInstallSnapshotResponse>();
        return response->SnapshotIndex > 0 && response->SnapshotIndex <= maxIndex &&
            response->NextOffset <= TSnapshot::MaxBytes && response->Success <= 1 && response->Installed <= 1 &&
            (!response->Installed || response->Success) && (response->Success || response->NextOffset == 0);
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
            if (entry->Flags & TLogEntry::EConfiguration) return false;
            if (!entry->Term || entry->Term < previousTerm || entry->Term > header->Term) return false;
            previousTerm = entry->Term;
            const auto index = append->PrevLogIndex + i + 1;
            if (index >= State->SnapshotIndex && index <= VolatileState->CommitIndex && State->LogTerm(index) != entry->Term) return false;
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
        if (!IsVoter(id)) continue;
        if (VolatileState->RpcDue[id] <= now) {
            VolatileState->RpcDue[id] = now + TTimeout::Rpc;
            node->Send(CreateVote(id));
        }
    }
}

void TRaft::LeaderTimeout(ITimeSource::Time now) {
    for (auto& [id, node] : Nodes) {
        if (VolatileState->HeartbeatDue[id] <= now
            || (VolatileState->NextIndex[id] <= State->LastLogIndex &&
            VolatileState->RpcDue[id] <= now))
        {
            VolatileState->HeartbeatDue[id] = now + TTimeout::Election / 2;
            VolatileState->RpcDue[id] = now + TTimeout::Rpc;
            if (VolatileState->NextIndex[id] <= State->SnapshotIndex) node->Send(CreateInstallSnapshot(id));
            else node->Send(CreateAppendEntries(id));
        }
    }

    if (Nservers == 1) {
        VolatileState->CommitAdvance(*VotingQuorum, Id, *State);
    }
}

uint64_t TRaft::ApproveRead() {
    if (StateName != EState::LEADER) throw std::logic_error("Only a leader can approve a read");
    // Reserve a unique barrier even for a single-node cluster. Every request
    // emitted below is newer than this barrier; old replies cannot satisfy it.
    const uint64_t seqno = NextSeqno();
    for (auto& [id, node] : Nodes) {
        if (VolatileState->NextIndex[id] <= State->SnapshotIndex) node->Send(CreateInstallSnapshot(id));
        else node->Send(CreateAppendEntries(id));
    }
    return seqno;
}

uint64_t TRaft::CommitSeqno() const {
    if (StateName != EState::LEADER || VolatileState->CommitIndex == 0 ||
        State->LogTerm(VolatileState->CommitIndex) != State->CurrentTerm) return 0;
    return VotingQuorum->Value([&](uint32_t id) -> uint64_t {
        if (id == Id) return Seqno - 1; // leader's own confirmation
        auto it = VolatileState->ReadAckSeqno.find(id);
        return it == VolatileState->ReadAckSeqno.end() ? 0 : it->second;
    });
}

uint64_t TRaft::NextSeqno() {
    if (Seqno == UINT64_MAX) throw std::overflow_error("Raft RPC sequence exhausted");
    return Seqno++;
}

void TRaft::ProcessTimeout(ITimeSource::Time now) {
    // Learners do not campaign, so their election timeout cannot be used to
    // reclaim an interrupted InstallSnapshot transfer. Expire partial images
    // independently of role and ordinary heartbeat activity.
    if (IncomingSnapshot && now >= IncomingSnapshot->LastProgress &&
        now - IncomingSnapshot->LastProgress >= TTimeout::SnapshotTransferIdle)
        IncomingSnapshot.reset();
    if (!IsVoter(Id)) return;
    if (StateName == EState::CANDIDATE || StateName == EState::FOLLOWER) {
        if (VolatileState->ElectionDue <= now) {
            if (State->CurrentTerm == UINT64_MAX) throw std::overflow_error("Raft term exhausted");
            IncomingSnapshot.reset();
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
        if (VotingQuorum->HasQuorum(VolatileState->Votes, Id)) {
            if (State->LastLogIndex >= uint64_t(INT64_MAX)) throw std::overflow_error("Raft log index exhausted");
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
    if (!IsVoter(Id)) throw std::logic_error("A learner cannot propose log entries");
    if (StateName != EState::LEADER) throw std::logic_error("Only a leader can propose log entries");
    if (State->LastLogIndex >= uint64_t(INT64_MAX)) throw std::overflow_error("Raft log index exhausted");
    if (!entry.HasValidExtent() || entry->Type != uint32_t(EMessageType::LOG_ENTRY))
        throw TMessageError("Invalid proposed log entry");
    ValidateMessageTree(entry);
    if (entry->Flags & TLogEntry::EConfiguration)
        throw TMessageError("Configuration proposals are unsupported");
    entry->Term = State->CurrentTerm;
    State->Append(std::move(entry));
    return State->LastLogIndex;
}

uint32_t TRaft::GetLeaderId() const {
    return VolatileState->LeaderId;
}

uint64_t TRaft::GetLastIndex() const {
    return State->LastLogIndex;
}

std::vector<uint32_t> TRaft::GetVoterIds() const { return Voters; }

bool TRaft::IsVoter(uint32_t id) const { return std::binary_search(Voters.begin(), Voters.end(), id); }

void TRaft::AddLearnerTarget(uint32_t id, std::shared_ptr<INode> node) {
    if (StateName != EState::LEADER) throw std::logic_error("Only a leader can stage a learner");
    if (!id || id == Id || !node || Nodes.contains(id) || IsVoter(id) ||
        std::binary_search(Learners.begin(), Learners.end(), id))
        throw std::invalid_argument("Invalid or duplicate learner target");
    if (!State->SupportsVoterBinding() || State->GetBoundVoters() != Voters)
        throw std::logic_error("Learner staging requires a bound voter configuration");
    if (Nodes.size() >= size_t(INT_MAX)) throw std::overflow_error("Too many replication targets");
    // Build all allocating changes before publishing any part of the target.
    auto nodes = Nodes;
    auto learners = Learners;
    auto nextIndex = VolatileState->NextIndex;
    nodes.emplace(id, std::move(node));
    learners.insert(std::lower_bound(learners.begin(), learners.end(), id), id);
    nextIndex.emplace(id, State->LastLogIndex + 1);
    Nodes.swap(nodes); Learners.swap(learners); VolatileState->NextIndex.swap(nextIndex);
    Npeers = int(Nodes.size());
}

std::shared_ptr<INode> TRaft::RemoveLearnerTarget(uint32_t id) {
    if (StateName != EState::LEADER) throw std::logic_error("Only a leader can remove a learner target");
    auto learner = std::lower_bound(Learners.begin(), Learners.end(), id);
    auto peer = Nodes.find(id);
    if (id == Id || learner == Learners.end() || *learner != id || peer == Nodes.end() || IsVoter(id))
        throw std::invalid_argument("Unknown learner target");
    auto node = peer->second;
    Nodes.erase(peer); Learners.erase(learner);
    VolatileState->NextIndex.erase(id); VolatileState->MatchIndex.erase(id);
    VolatileState->ReadAckSeqno.erase(id); VolatileState->LastSentSeqno.erase(id);
    VolatileState->PendingAppendRpcs.erase(id); VolatileState->SnapshotRpcs.erase(id);
    VolatileState->HeartbeatDue.erase(id); VolatileState->RpcDue.erase(id);
    VolatileState->BatchSize.erase(id); VolatileState->BackOff.erase(id);
    VolatileState->Votes.erase(id);
    Npeers = int(Nodes.size());
    return node;
}

bool TRaft::IsLearnerCaughtUp(uint32_t id) const {
    if (StateName != EState::LEADER || !std::binary_search(Learners.begin(), Learners.end(), id) ||
        VolatileState->CommitIndex == 0 || State->LogTerm(VolatileState->CommitIndex) != State->CurrentTerm) return false;
    auto match = VolatileState->MatchIndex.find(id);
    return match != VolatileState->MatchIndex.end() && match->second >= State->LastLogIndex;
}

TSnapshot TRaft::Compact() {
    if (!State->SupportsSnapshots()) throw std::logic_error("Storage cannot compact snapshots");
    auto application = SnapshotApplication.lock();
    if (!application) throw std::logic_error("Compaction requires a bound application");
    if (application->LastAppliedIndex == State->SnapshotIndex && State->GetSnapshot()) return *State->GetSnapshot();
    std::unordered_set<uint64_t> pinnedGenerations;
    for (const auto& [_, rpc] : VolatileState->SnapshotRpcs)
        if (rpc.Snapshot) pinnedGenerations.insert(rpc.Snapshot->Index);
    if (pinnedGenerations.size() >= TVolatileState::MaxPinnedSnapshotGenerations)
        throw std::runtime_error("Compaction deferred while snapshot transfers pin multiple generations");
    auto snapshot = CaptureSnapshot(*this, *application);
    application->ValidateSnapshot(snapshot.Data, snapshot.Index);
    State->InstallSnapshot(snapshot);
    // Outstanding transfers own their immutable encoded bytes; discard any
    // otherwise-retained encoding of the old local snapshot.
    EncodedSnapshot.reset();
    EncodedSnapshotIndex = 0;
    return snapshot;
}

TMessageHolder<TInstallSnapshotRequest> TRaft::CreateInstallSnapshot(uint32_t nodeId) {
    auto& rpc = VolatileState->SnapshotRpcs[nodeId];
    if (!rpc.Snapshot) {
        auto snapshot = State->GetSnapshot();
        if (!snapshot) throw std::logic_error("Missing snapshot for compacted log");
        if (EncodedSnapshotIndex != snapshot->Index) {
            EncodedSnapshot = std::make_shared<const std::string>(EncodeSnapshot(*snapshot));
            EncodedSnapshotIndex = snapshot->Index;
        }
        rpc = TVolatileState::TSnapshotRpc{snapshot, EncodedSnapshot};
        VolatileState->PendingAppendRpcs[nodeId].clear();
    }
    // Pin one immutable image until this peer finishes. Periodic compaction
    // must not restart every chunk; after completion a still-lagging peer can
    // start a separate transfer of the newest image.
    const auto& snapshot = rpc.Snapshot;
    const auto count = std::min<uint64_t>(TInstallSnapshotRequest::MaxChunkBytes, rpc.Bytes->size() - rpc.Offset);
    auto request = NewHoldedMessage<TInstallSnapshotRequest>(sizeof(TInstallSnapshotRequest) + count);
    request->Src = Id; request->Dst = nodeId; request->Term = State->CurrentTerm;
    // A retry sends the same immutable chunk. Keep its identity until a reply
    // consumes it, otherwise traffic faster than the RTT invalidates every ACK.
    // Snapshot ACKs never confirm read barriers, so reuse cannot confirm a read
    // that arrived after the first transmission.
    if (!rpc.Seqno) rpc.Seqno = NextSeqno();
    request->Seqno = rpc.Seqno;
    request->SnapshotIndex = snapshot->Index; request->SnapshotTerm = snapshot->Term;
    request->Offset = rpc.Offset; request->TotalBytes = rpc.Bytes->size();
    rpc.SentEnd = rpc.Offset + count;
    request->Done = rpc.SentEnd == rpc.Bytes->size(); request->LeaderId = Id;
    memcpy(request->Data, rpc.Bytes->data() + rpc.Offset, count);
    return request;
}

void TRaft::OnInstallSnapshot(TMessageHolder<TInstallSnapshotResponse> response) {
    if (response->Term != State->CurrentTerm) return;
    auto found = VolatileState->SnapshotRpcs.find(response->Src);
    if (found == VolatileState->SnapshotRpcs.end()) return;
    auto& rpc = found->second;
    if (!rpc.Seqno || response->Seqno != rpc.Seqno || response->SnapshotIndex != rpc.Snapshot->Index) return;
    if (!response->Success) {
        rpc.Offset = 0;
        rpc.Seqno = 0;
    } else if (response->Installed) {
        if (response->NextOffset != rpc.Bytes->size()) return;
        const auto index = std::max(VolatileState->MatchIndex[response->Src], response->SnapshotIndex);
        (*VolatileState).SetMatchIndex(response->Src, index).SetNextIndex(response->Src, index + 1)
            .SetBatchSize(response->Src, 1024).SetBackOff(response->Src, 1).CommitAdvance(*VotingQuorum, Id, *State);
        VolatileState->SnapshotRpcs.erase(found);
        // Snapshot completion is not a fresh-read acknowledgement. AppendEntries
        // must still establish the next read barrier and replicate the suffix.
    } else {
        if (response->NextOffset != rpc.SentEnd || response->NextOffset >= rpc.Bytes->size()) return;
        rpc.Offset = response->NextOffset;
        rpc.Seqno = 0; // Next chunk gets a new identity; old replies stay stale.
    }
    VolatileState->RpcDue[response->Src] = ITimeSource::Time{};
}

void TRaft::OnInstallSnapshot(ITimeSource::Time now, TMessageHolder<TInstallSnapshotRequest> request) {
    auto reply = [&](bool success, bool installed, uint64_t offset) {
        Nodes.at(request->Src)->Send(NewHoldedMessage(
            TMessageEx{.Src = Id, .Dst = request->Src, .Term = State->CurrentTerm, .Seqno = request->Seqno},
            TInstallSnapshotResponse{.SnapshotIndex = request->SnapshotIndex, .NextOffset = offset,
                .Success = uint32_t(success), .Installed = uint32_t(installed)}));
    };
    if (request->Term < State->CurrentTerm) { reply(false, false, 0); return; }
    Become(EState::FOLLOWER);
    VolatileState->LeaderId = request->LeaderId;
    VolatileState->ElectionDue = MakeElection(now);
    if (request->SnapshotIndex <= VolatileState->CommitIndex) {
        const auto term = State->LogTerm(request->SnapshotIndex);
        const bool covered = request->SnapshotIndex < State->SnapshotIndex || term == request->SnapshotTerm;
        if (!covered) { reply(false, false, 0); return; }
        // A covered image needs no application replacement, but its snapshot
        // boundary and fixed voter set must match before it can count as
        // replicated. Accumulate only the bounded metadata prefix.
        const bool same = IncomingSnapshot && IncomingSnapshot->Term == request->Term &&
            IncomingSnapshot->Leader == request->LeaderId && IncomingSnapshot->Index == request->SnapshotIndex &&
            IncomingSnapshot->BoundaryTerm == request->SnapshotTerm && IncomingSnapshot->Total == request->TotalBytes;
        if (!same) {
            if (request->Offset != 0 || (IncomingSnapshot && IncomingSnapshot->Term == request->Term &&
                IncomingSnapshot->Index > request->SnapshotIndex)) { reply(false, false, 0); return; }
            IncomingSnapshot = TIncomingSnapshot{request->Term, request->SnapshotIndex, request->SnapshotTerm,
                request->TotalBytes, request->LeaderId, now, {}};
        }
        auto& prefix = IncomingSnapshot->Bytes;
        const auto count = request->Len - sizeof(TInstallSnapshotRequest);
        if (request->Offset > prefix.size()) { reply(false, false, 0); return; }
        if (request->Offset < prefix.size()) {
            if (count > prefix.size() - request->Offset ||
                std::string_view(prefix).substr(request->Offset, count) != std::string_view(request->Data, count)) {
                IncomingSnapshot.reset(); reply(false, false, 0); return;
            }
        } else prefix.append(request->Data, count);
        IncomingSnapshot->LastProgress = now;
        const auto knownSnapshot = State->GetSnapshot();
        const auto probe = ProbeSnapshotMetadata(prefix, request->TotalBytes,
            request->SnapshotIndex, request->SnapshotTerm, GetVoterIds(), knownSnapshot.get());
        if (!probe.Complete) { reply(true, false, prefix.size()); return; }
        IncomingSnapshot.reset();
        reply(probe.Supported, probe.Supported, probe.Supported ? request->TotalBytes : 0);
        return;
    }
    auto application = SnapshotApplication.lock();
    if (!State->SupportsSnapshots() || !application) { reply(false, false, 0); return; }
    const bool same = IncomingSnapshot && IncomingSnapshot->Term == request->Term &&
        IncomingSnapshot->Leader == request->LeaderId && IncomingSnapshot->Index == request->SnapshotIndex &&
        IncomingSnapshot->BoundaryTerm == request->SnapshotTerm && IncomingSnapshot->Total == request->TotalBytes;
    if (!same) {
        if (request->Offset != 0 || (IncomingSnapshot && IncomingSnapshot->Term == request->Term &&
            IncomingSnapshot->Index > request->SnapshotIndex)) { reply(false, false, 0); return; }
        IncomingSnapshot = TIncomingSnapshot{request->Term, request->SnapshotIndex, request->SnapshotTerm,
            request->TotalBytes, request->LeaderId, now, {}};
    }
    auto& bytes = IncomingSnapshot->Bytes;
    const auto count = request->Len - sizeof(TInstallSnapshotRequest);
    if (request->Offset > bytes.size()) { reply(false, false, 0); return; }
    if (request->Offset < bytes.size()) {
        if (count > bytes.size() - request->Offset ||
            std::string_view(bytes).substr(request->Offset, count) != std::string_view(request->Data, count)) {
            IncomingSnapshot.reset(); reply(false, false, 0); return;
        }
    } else bytes.append(request->Data, count);
    IncomingSnapshot->LastProgress = now;
    if (bytes.size() >= 8 && std::string_view(bytes).substr(0, 8) != "MRFTS001" &&
        std::string_view(bytes).substr(0, 8) != "MRFTS003") {
        IncomingSnapshot.reset(); reply(false, false, 0); return;
    }
    if (bytes.size() < request->TotalBytes) { reply(true, false, bytes.size()); return; }
    TSnapshot snapshot;
    try {
        snapshot = DecodeSnapshot(bytes);
        if (snapshot.IsJoint() || snapshot.Index != request->SnapshotIndex || snapshot.Term != request->SnapshotTerm || snapshot.Voters != GetVoterIds())
            throw TMessageError("Snapshot metadata mismatch");
        const auto knownSnapshot = State->GetSnapshot();
        PreserveStableSnapshotIdentity(knownSnapshot.get(), snapshot);
        ValidateSnapshot(snapshot);
        application->ValidateSnapshot(snapshot.Data, snapshot.Index);
    } catch (const std::exception&) {
        IncomingSnapshot.reset(); reply(false, false, 0); return;
    }
    try {
        State->InstallSnapshot(snapshot); // durable snapshot and suffix before application replacement or ACK
        application->LoadSnapshot(snapshot.Data, snapshot.Index);
        if (application->LastAppliedIndex != snapshot.Index) std::terminate();
    } catch (...) { std::terminate(); }
    VolatileState->CommitIndex = snapshot.Index;
    IncomingSnapshot.reset();
    reply(true, true, request->TotalBytes);
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
    if (auto snapshot = Raft->GetState()->GetSnapshot(); snapshot && Rsm->LastAppliedIndex < snapshot->Index)
        RestoreSnapshot(*snapshot, *Raft, *Rsm);
    if (Rsm->LastAppliedIndex < Raft->GetState()->SnapshotIndex)
        throw std::invalid_argument("Application progress precedes compacted log");
    Raft->RestoreAppliedIndex(Rsm->LastAppliedIndex);
    Raft->SnapshotApplication = Rsm;
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
    if (!command.HasValidExtent() || command->Type != uint32_t(EMessageType::COMMAND_REQUEST))
        throw TMessageError("Invalid command request");
    ValidateMessageTree(command);
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
    if (!command.HasValidExtent() || command->Type != uint32_t(EMessageType::COMMAND_RESPONSE))
        throw TMessageError("Invalid command response");
    ValidateMessageTree(command);
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
    auto commitIndex = Raft->GetVolatileState()->CommitIndex;
    // Never rewind application progress, or overflow LastAppliedIndex + 1.
    if (commitIndex <= Rsm->LastAppliedIndex) return;
    auto& state = Raft->GetState();
    for (auto i = Rsm->LastAppliedIndex+1; i <= commitIndex; i++) {
        auto entry = state->Get(i);
        if (entry->Flags & TLogEntry::EConfiguration)
            throw std::logic_error("Configuration application is unsupported");
        if (entry->Flags == TLogEntry::EStub) {
            continue;
        }
        TMessageHolder<TMessage> reply;
        try { reply = Rsm->Write(entry, i); }
        catch (...) { std::terminate(); }
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

