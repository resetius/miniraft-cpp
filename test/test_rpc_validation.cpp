#include <miniraft/raft.h>
#include <functional>
#include <gtest/gtest.h>

namespace {
struct TEndpoint : INode {
    std::vector<TMessageHolder<TMessage>> Sent;
    void Send(TMessageHolder<TMessage> message) override { Sent.push_back(message); }
    void Drain() override {}
};
struct TFixture {
    std::shared_ptr<TState> State = std::make_shared<TState>();
    std::shared_ptr<TEndpoint> Peer = std::make_shared<TEndpoint>();
    std::shared_ptr<TEndpoint> Other = std::make_shared<TEndpoint>();
    TRaft Raft{State, 1, {{2, Peer}, {3, Other}}};
    ITimeSource::Time Now{};
    TFixture() { State->CurrentTerm = 3; State->VotedFor = 1; }
    void Append(uint64_t term) {
        auto entry = NewHoldedMessage<TLogEntry>();
        entry->Term = term;
        State->Append(entry);
    }
    TMessageHolder<TAppendEntriesRequest> Send() {
        Raft.Become(EState::LEADER);
        Raft.ProcessTimeout(Now);
        return Peer->Sent.back().Cast<TAppendEntriesRequest>();
    }
    TMessageHolder<TAppendEntriesResponse> Ack(uint64_t sequence, uint64_t index, bool success = true) {
        return NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 3, .Seqno = sequence},
            TAppendEntriesResponse{.MatchIndex = index, .Success = uint32_t(success)});
    }
    TMessageHolder<TAppendEntriesRequest> Request(uint64_t prev, uint64_t prevTerm,
                                                  uint64_t commit, std::initializer_list<uint64_t> terms = {}) {
        auto request = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 4},
            TAppendEntriesRequest{.PrevLogIndex = prev, .PrevLogTerm = prevTerm,
                                  .LeaderCommit = commit, .LeaderId = 2, .Nentries = uint32_t(terms.size())});
        request.InitPayload(terms.size());
        uint32_t i = 0;
        for (auto term : terms) {
            auto entry = NewHoldedMessage<TLogEntry>();
            entry->Term = term;
            request.Payload[i++] = entry;
        }
        return request;
    }
};
TEST(RpcValidation, unknown_sender_cannot_change_term) {
    TFixture fixture;
    auto response = NewHoldedMessage(TMessageEx{.Src = 99, .Dst = 1, .Term = 100},
                                    TRequestVoteResponse{.VoteGranted = 1});
    fixture.Raft.Process(fixture.Now, response);
    ASSERT_EQ(fixture.State->CurrentTerm, 3);
    ASSERT_EQ(fixture.State->VotedFor, 1);
}
TEST(RpcValidation, wrong_destination_cannot_change_term) {
    TFixture fixture;
    auto response = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 3, .Term = 100},
                                    TRequestVoteResponse{.VoteGranted = 1});
    fixture.Raft.Process(fixture.Now, response);
    ASSERT_EQ(fixture.State->CurrentTerm, 3);
}
TEST(RpcValidation, candidate_identity_must_match_sender) {
    TFixture fixture;
    auto request = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 4},
        TRequestVoteRequest{.LastLogIndex = 0, .LastLogTerm = 0, .CandidateId = 3});
    fixture.Raft.Process(fixture.Now, request);
    ASSERT_EQ(fixture.State->CurrentTerm, 3);
    ASSERT_EQ(fixture.State->VotedFor, 1);
    ASSERT_TRUE(fixture.Peer->Sent.empty());
}
TEST(RpcValidation, out_of_range_ack_cannot_wrap_next_index) {
    TFixture fixture;
    fixture.Append(3);
    auto request = fixture.Send();
    fixture.Raft.Process(fixture.Now, fixture.Ack(request->Seqno, UINT64_MAX));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->NextIndex.at(2), 1);
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 0);
    fixture.Raft.Process(fixture.Now, fixture.Ack(request->Seqno, 1));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->NextIndex.at(2), 2);
}
TEST(RpcValidation, unknown_sequence_cannot_commit) {
    TFixture fixture;
    fixture.Append(3);
    auto request = fixture.Send();
    fixture.Raft.Process(fixture.Now, fixture.Ack(request->Seqno + 100, 1));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 0);
    fixture.Raft.Process(fixture.Now, fixture.Ack(request->Seqno, 1));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 1);
}
TEST(RpcValidation, ack_cannot_confirm_an_entry_not_in_its_rpc) {
    TFixture fixture;
    fixture.Append(1);
    fixture.Append(3);
    auto request = fixture.Send();
    ASSERT_EQ(request->Nentries, 1);
    fixture.Raft.Process(fixture.Now, fixture.Ack(request->Seqno, 2));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 0);
    ASSERT_EQ(fixture.Raft.GetVolatileState()->NextIndex.at(2), 1);
}
TEST(RpcValidation, old_failure_cannot_regress_new_success) {
    TFixture fixture;
    fixture.Append(3);
    auto first = fixture.Send();
    fixture.Now += TTimeout::Rpc + std::chrono::milliseconds(1);
    auto second = fixture.Send();
    fixture.Raft.Process(fixture.Now, fixture.Ack(second->Seqno, 1));
    fixture.Raft.Process(fixture.Now, fixture.Ack(first->Seqno, 0, false));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->NextIndex.at(2), 2);
    ASSERT_EQ(fixture.Raft.GetVolatileState()->BatchSize.at(2), 1024);
}
TEST(RpcValidation, duplicate_failure_does_not_repeat_backoff) {
    TFixture fixture;
    fixture.Append(3);
    auto request = fixture.Send();
    auto response = fixture.Ack(request->Seqno, 0, false);
    fixture.Raft.Process(fixture.Now, response);
    auto backoff = fixture.Raft.GetVolatileState()->BackOff.at(2);
    fixture.Raft.Process(fixture.Now, response);
    ASSERT_EQ(fixture.Raft.GetVolatileState()->BackOff.at(2), backoff);
}
TEST(RpcValidation, follower_commits_only_prefix_matched_by_rpc) {
    TFixture fixture;
    fixture.Append(1);
    fixture.Append(2);
    fixture.Append(2);
    auto request = fixture.Request(1, 1, 9);
    fixture.Raft.Process(fixture.Now, request);
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 1);
    ASSERT_EQ(fixture.State->LastLogIndex, 3);
}
TEST(RpcValidation, commit_index_preserves_full_width) {
    TVolatileState state;
    const uint64_t index = (uint64_t(1) << 32) + 123;
    state.SetCommitIndex(index);
    ASSERT_EQ(state.CommitIndex, index);
}
TEST(RpcValidation, invalid_batch_terms_do_not_partially_mutate_log) {
    TFixture fixture;
    fixture.Append(1);
    auto request = fixture.Request(1, 1, 0, {3, 2});
    fixture.Raft.Process(fixture.Now, request);
    ASSERT_EQ(fixture.State->CurrentTerm, 3);
    ASSERT_EQ(fixture.State->LastLogIndex, 1);
}
TEST(RpcValidation, committed_prefix_cannot_be_replaced) {
    TFixture fixture;
    fixture.Append(1);
    fixture.Append(2);
    const_cast<TVolatileState*>(fixture.Raft.GetVolatileState())->CommitIndex = 2;
    fixture.Raft.Process(fixture.Now, fixture.Request(1, 1, 2, {3}));
    ASSERT_EQ(fixture.State->LogTerm(2), 2);
    ASSERT_EQ(fixture.State->CurrentTerm, 3);
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 2);
}
TEST(RpcValidation, impossible_vote_log_metadata_is_ignored) {
    for (int invalid = 0; invalid < 3; ++invalid) {
        TFixture fixture;
        auto request = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 4},
            TRequestVoteRequest{.LastLogIndex = 1, .LastLogTerm = 3, .CandidateId = 2});
        if (invalid == 0) request->LastLogIndex = UINT64_MAX;
        if (invalid == 1) request->LastLogTerm = 5;
        if (invalid == 2) request->LastLogIndex = 0;
        fixture.Raft.Process(fixture.Now, request);
        ASSERT_EQ(fixture.State->CurrentTerm, 3);
        ASSERT_EQ(fixture.State->VotedFor, 1);
    }
}
TEST(RpcValidation, invalid_peer_configuration_is_rejected) {
    auto state = std::make_shared<TState>();
    auto peer = std::make_shared<TEndpoint>();
    for (int invalid = 0; invalid < 4; ++invalid) {
        bool rejected = false;
        try {
            if (invalid == 0) { TRaft raft(state, 0, {}); }
            if (invalid == 1) { TRaft raft(state, 1, {{0, peer}}); }
            if (invalid == 2) { TRaft raft(state, 1, {{1, peer}}); }
            if (invalid == 3) { TRaft raft(state, 1, {{2, {}}}); }
        } catch (const std::invalid_argument&) { rejected = true; }
        ASSERT_TRUE(rejected);
    }
}
TEST(RpcValidation, invalid_requests_and_boolean_fields_are_ignored) {
    for (int invalid = 0; invalid < 8; ++invalid) {
        TFixture fixture;
        fixture.Append(1);
        auto request = fixture.Request(1, 1, 0, {3});
        if (invalid == 0) request->Src = 99;
        if (invalid == 1) request->LeaderId = 3;
        if (invalid == 2) request->PrevLogIndex = UINT64_MAX;
        if (invalid == 3) request->LeaderCommit = UINT64_MAX;
        if (invalid == 4) request.Payload[0].Cast<TLogEntry>()->Term = 0;
        if (invalid == 5) request.Payload[0].Cast<TLogEntry>()->Term = 5;
        if (invalid == 6) request->Nentries = 2;
        if (invalid == 7) request->PrevLogIndex = uint64_t(INT64_MAX);
        fixture.Raft.Process(fixture.Now, request);
        ASSERT_EQ(fixture.State->CurrentTerm, 3);
        ASSERT_EQ(fixture.State->LastLogIndex, 1);
        ASSERT_TRUE(fixture.Peer->Sent.empty());
    }
    TFixture fixture;
    auto vote = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 100},
                                 TRequestVoteResponse{.VoteGranted = 2});
    fixture.Raft.Process(fixture.Now, vote);
    ASSERT_EQ(fixture.State->CurrentTerm, 3);
    auto append = fixture.Ack(1, 0);
    append->Term = 100;
    append->Success = 2;
    fixture.Raft.Process(fixture.Now, append);
    ASSERT_EQ(fixture.State->CurrentTerm, 3);
}
TEST(RpcValidation, old_evicted_response_is_ignored_and_latest_progresses) {
    TFixture fixture;
    fixture.Append(3);
    auto first = fixture.Send();
    for (int i = 0; i < 300; ++i) {
        fixture.Now += TTimeout::Rpc + std::chrono::milliseconds(1);
        fixture.Send();
    }
    auto latest = fixture.Peer->Sent.back().Cast<TAppendEntriesRequest>();
    fixture.Raft.Process(fixture.Now, fixture.Ack(first->Seqno, 1));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 0);
    fixture.Raft.Process(fixture.Now, fixture.Ack(latest->Seqno, 1));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->CommitIndex, 1);
}
TEST(RpcValidation, term_exhaustion_does_not_wrap_or_change_vote) {
    TFixture fixture;
    fixture.State->CurrentTerm = UINT64_MAX;
    bool rejected = false;
    try { fixture.Raft.ProcessTimeout(fixture.Now); }
    catch (const std::overflow_error&) { rejected = true; }
    ASSERT_TRUE(rejected);
    ASSERT_EQ(fixture.State->CurrentTerm, UINT64_MAX);
    ASSERT_EQ(fixture.State->VotedFor, 1);
    ASSERT_EQ(fixture.Raft.CurrentStateName(), EState::FOLLOWER);
}
TEST(RpcValidation, lower_inflight_probe_cannot_regress_matched_progress) {
    TFixture fixture;
    fixture.Append(3);
    fixture.Append(3);
    auto* progress = const_cast<TVolatileState*>(fixture.Raft.GetVolatileState());
    progress->NextIndex[2] = 3; // newly elected leader initially probes its tail
    auto first = fixture.Send();
    fixture.Now += TTimeout::Rpc + std::chrono::milliseconds(1);
    auto higher = fixture.Send();
    fixture.Raft.Process(fixture.Now, fixture.Ack(first->Seqno, 0, false));
    fixture.Now += TTimeout::Rpc + std::chrono::milliseconds(1);
    auto lower = fixture.Send();
    ASSERT_EQ(higher->PrevLogIndex, 2);
    ASSERT_EQ(lower->PrevLogIndex, 1);
    fixture.Raft.Process(fixture.Now, fixture.Ack(higher->Seqno, 2));
    fixture.Raft.Process(fixture.Now, fixture.Ack(lower->Seqno, 1));
    ASSERT_EQ(fixture.Raft.GetVolatileState()->MatchIndex.at(2), 2);
    ASSERT_EQ(fixture.Raft.GetVolatileState()->NextIndex.at(2), 3);
}
}
