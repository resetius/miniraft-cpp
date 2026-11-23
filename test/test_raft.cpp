#include <chrono>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <memory>
#include <functional>

#include <miniraft/messages.h>
#include <miniraft/raft.h>
#include <miniraft/timesource.h>
#include <miniraft/persist.h>

#include <gtest/gtest.h>

namespace {

using OnSendFunc = const std::function<void(TMessageHolder<TMessage>)>;

class TFakeNode: public INode {
public:
    TFakeNode(const OnSendFunc& sendFunc = {})
        : SendFunc(sendFunc)
    { }

    void Send(TMessageHolder<TMessage> message) override {
        if (SendFunc) {
            SendFunc(std::move(message));
        }
    }

    void Drain() override { }

private:
    OnSendFunc SendFunc;
};

class TFakeTimeSource: public ITimeSource {
public:
    TFakeTimeSource()
        : T(std::chrono::steady_clock::now())
    { }

    ITimeSource::Time Now() override {
        return T;
    }

    template<typename A, typename B>
    void Advance(std::chrono::duration<A, B> duration) {
        T += duration;
    }

private:
    ITimeSource::Time T;
};

std::shared_ptr<TRaft> MakeRaft(
    const OnSendFunc& sendFunc = {},
    int count = 3,
    TState st = {})
{
    TNodeDict nodes;
    for (int i = 2; i <= count; i++) {
        nodes[i] = std::make_shared<TFakeNode>(sendFunc);
    }
    std::shared_ptr<IState> state = std::make_shared<TState>(st);
    return std::make_shared<TRaft>(std::move(state), 1, nodes);
}

template<typename T=TMessage>
std::vector<TMessageHolder<T>> MakeLog(const std::vector<uint64_t>& terms) {
    std::vector<TMessageHolder<T>> entries;
    for (auto term : terms) {
        auto mes = NewHoldedMessage<TLogEntry>();
        mes->Term = term;
        entries.push_back(mes);
    }
    return entries;
}

template<typename T>
void SetPayload(TMessageHolder<T>& dst, const std::vector<TMessageHolder<TMessage>>& payload) {
    dst.InitPayload(payload.size());
    for (uint32_t i = 0; i < payload.size(); ++i) {
        dst.Payload[i] = payload[i];
    }
}

void assert_terms(const std::vector<TMessageHolder<TLogEntry>>& log, const std::vector<uint64_t>& terms)
{
    ASSERT_EQ(log.size(), terms.size());
    for (size_t i = 0; i < log.size(); i++) {
        ASSERT_EQ(log[i]->Term, terms[i]);
    }
}

template<typename T>
void assert_message_equal(TMessageHolder<TMessage> m1, const T& m2) {
    auto mm = m1.Maybe<T>();
    assert(mm);
    auto m = mm.Cast();
    if (memcmp(m.Mes, &m2, sizeof(T)) != 0) {
        const char* p1 = (const char*)m.Mes;
        const char* p2 = (const char*)&m2;
        ASSERT_EQ(0, memcmp(p1, p2, sizeof(m2)));
    }
}

} // namespace {

TEST(Raft, empty) {

}

TEST(Raft, message_create) {
    auto mes = NewHoldedMessage<TMessage>(0, 8);
    ASSERT_TRUE(mes->Len == 8);
    ASSERT_TRUE(mes->Type == 0);
}

TEST(Raft, message_cast) {
    TMessageHolder<TMessage> mes = NewHoldedMessage(TLogEntry{});
    auto casted = mes.Cast<TLogEntry>();
    ASSERT_TRUE(mes.RawData == casted.RawData);
    ASSERT_TRUE(mes->Len == casted->Len);

    auto casted2 = mes.Maybe<TRequestVoteRequest>();
    ASSERT_FALSE(casted2);

    auto maybeCasted = mes.Maybe<TLogEntry>();
    ASSERT_TRUE(maybeCasted);
    auto casted3 = maybeCasted.Cast();
    ASSERT_TRUE(mes.RawData == casted3.RawData);
    ASSERT_TRUE(mes->Len == casted3->Len);
}

TEST(Raft, initial) {
    auto raft = MakeRaft();
    ASSERT_TRUE(raft->CurrentStateName() == EState::FOLLOWER);
}

TEST(Raft, numbers) {
    auto raft = MakeRaft({}, 3);
    ASSERT_EQ(raft->GetMinVotes(), 2);
    ASSERT_EQ(raft->GetNservers(), 3);
    ASSERT_EQ(raft->GetNpeers(), 2);

    raft = MakeRaft({}, 2);
    ASSERT_EQ(raft->GetMinVotes(), 2);
    ASSERT_EQ(raft->GetNservers(), 2);
    ASSERT_EQ(raft->GetNpeers(), 1);

    raft = MakeRaft({}, 1);
    ASSERT_EQ(raft->GetMinVotes(), 1);
    ASSERT_EQ(raft->GetNservers(), 1);
    ASSERT_EQ(raft->GetNpeers(), 0);

    raft = MakeRaft({}, 5);
    ASSERT_EQ(raft->GetMinVotes(), 3);
    ASSERT_EQ(raft->GetNservers(), 5);
    ASSERT_EQ(raft->GetNpeers(), 4);

    raft = MakeRaft({}, 10);
    ASSERT_EQ(raft->GetMinVotes(), 6);
    ASSERT_EQ(raft->GetNservers(), 10);
    ASSERT_EQ(raft->GetNpeers(), 9);
}

TEST(Raft, become) {
    auto raft = MakeRaft();
    ASSERT_TRUE(raft->CurrentStateName() == EState::FOLLOWER);
    raft->Become(EState::CANDIDATE);
    ASSERT_TRUE(raft->CurrentStateName() == EState::CANDIDATE);
}

TEST(Raft, become_same_func) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3);
    ASSERT_TRUE(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::FOLLOWER);
    ASSERT_TRUE(raft->CurrentStateName() == EState::FOLLOWER);
}

TEST(Raft, follower_to_candidate_on_timeout) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 3);
    ASSERT_TRUE(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->ProcessTimeout(ts->Now());
    ASSERT_TRUE(raft->CurrentStateName() == EState::CANDIDATE);
}

TEST(Raft, follower_append_entries_small_term) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 0,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 0,
        .PrevLogTerm = 0,
        .LeaderCommit = 0,
        .LeaderId = 2,
        .Nentries = 0,
    });
    raft->Process(ts->Now(), mes);

    ASSERT_TRUE(messages.size() == 1);
    auto maybeReply = messages[0].Maybe<TAppendEntriesResponse>();
    ASSERT_TRUE(maybeReply);
    auto reply = maybeReply.Cast();
    ASSERT_TRUE(reply->Dst == 2);
    ASSERT_FALSE(reply->Success);
}

TEST(Raft, stale_append_entries_does_not_postpone_election) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    // start an election, current term becomes 2, election due is in the future
    raft->ProcessTimeout(ts->Now());
    ASSERT_TRUE(raft->CurrentStateName() == EState::CANDIDATE);
    auto term = raft->GetState()->CurrentTerm;
    auto electionDue = raft->GetVolatileState()->ElectionDue;
    ASSERT_TRUE(electionDue > ts->Now());

    // a stale leader keeps sending append entries with an outdated term
    for (int i = 0; i < 10; i++) {
        ts->Advance(std::chrono::milliseconds(100));
        auto mes = NewHoldedMessage(TMessageEx {
            .Src = 2,
            .Dst = 1,
            .Term = term - 1,
        }, TAppendEntriesRequest {
            .PrevLogIndex = 0,
            .PrevLogTerm = 0,
            .LeaderCommit = 0,
            .LeaderId = 2,
            .Nentries = 0,
        });
        raft->Process(ts->Now(), mes);
        raft->ProcessTimeout(ts->Now());
        ASSERT_TRUE(raft->GetVolatileState()->ElectionDue == electionDue);
        ASSERT_EQ(raft->GetState()->CurrentTerm, term);
    }

    // the original deadline still starts the next election round
    ts->Advance(std::chrono::duration_cast<std::chrono::milliseconds>(electionDue - ts->Now()));
    raft->ProcessTimeout(ts->Now());
    ASSERT_EQ(raft->GetState()->CurrentTerm, term + 1);
    ASSERT_TRUE(raft->CurrentStateName() == EState::CANDIDATE);
}

TEST(Raft, follower_append_entries_7a) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 6,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 6,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 9,
        .PrevLogTerm = 6,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 1,
    });
    SetPayload(mes, MakeLog({6}));
    raft->Process(ts->Now(), mes);
    ASSERT_EQ(messages.size(), 1);
    ASSERT_TRUE(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    ASSERT_TRUE(last->Success);
    ASSERT_TRUE(last->MatchIndex = 10);
    ASSERT_TRUE(raft->GetState()->LastLogIndex == 10);
}

TEST(Raft, follower_append_entries_7b) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 6,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,4})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 6,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 4,
        .PrevLogTerm = 4,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 6,
    });
    SetPayload(mes, MakeLog({4,5,5,6,6,6}));
    raft->Process(ts->Now(), mes);
    ASSERT_EQ(messages.size(), 1);
    ASSERT_TRUE(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    ASSERT_TRUE(last->Success);
    ASSERT_TRUE(last->MatchIndex = 10);
    ASSERT_TRUE(raft->GetState()->LastLogIndex == 10);
}

TEST(Raft, follower_append_entries_7c) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 6,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6,6,6})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 6,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 9,
        .PrevLogTerm = 6,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 1,
    });
    SetPayload(mes, MakeLog({6}));
    raft->Process(ts->Now(), mes);
    ASSERT_EQ(messages.size(), 1);
    ASSERT_TRUE(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    ASSERT_TRUE(last->Success);
    ASSERT_TRUE(last->MatchIndex = 10);
    ASSERT_TRUE(raft->GetState()->LastLogIndex == 11);
}

TEST(Raft, follower_append_entries_7f) {
    // leader: 1,1,1,4,4,5,5,6,6,6
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](const TMessageHolder<TMessage>& message) {
        messages.push_back(message);
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3, TState{
        /*.CurrentTerm =*/ 3,
        /*.VotedFor =*/ 2,
        /*.Log =*/ MakeLog<TLogEntry>({1,1,1,2,2,2,3,3,3,3,3})
    });
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 8,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 3,
        .PrevLogTerm = 1,
        .LeaderCommit = 9,
        .LeaderId = 2,
        .Nentries = 7,
    });
    SetPayload(mes, MakeLog({4,4,5,5,6,6,6}));
    raft->Process(ts->Now(), mes);
    ASSERT_EQ(messages.size(), 1);
    ASSERT_TRUE(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    ASSERT_TRUE(last->Success);
    ASSERT_TRUE(last->MatchIndex = 10);
    ASSERT_TRUE(raft->GetState()->LastLogIndex == 10);
    assert_terms(std::static_pointer_cast<TState>(raft->GetState())->Log, {1,1,1,4,4,5,5,6,6,6});
}

TEST(Raft, follower_append_entries_empty_to_empty_log) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto mes = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 1,
    }, TAppendEntriesRequest {
        .PrevLogIndex = 0,
        .PrevLogTerm = 0,
        .LeaderCommit = 0,
        .LeaderId = 2,
        .Nentries = 0,
    });
    raft->Process(ts->Now(), mes);
    ASSERT_EQ(messages.size(), 1);
    ASSERT_TRUE(messages.back().Maybe<TAppendEntriesResponse>());
    auto last = messages.back().Cast<TAppendEntriesResponse>();
    ASSERT_EQ(last->Dst, 2);
    ASSERT_TRUE(last->Success);
    ASSERT_EQ(last->MatchIndex, 0);
}

TEST(Raft, candidate_initiate_election) {
    std::vector<TMessageHolder<TRequestVoteRequest>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(message.template Cast<TRequestVoteRequest>());
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    ts->Advance(std::chrono::milliseconds(10000));
    auto term = raft->GetState()->CurrentTerm;
    raft->Become(EState::CANDIDATE);
    raft->ProcessTimeout(ts->Now());
    ASSERT_EQ(raft->GetState()->CurrentTerm, term+1);
    ASSERT_EQ(messages.size(), 2);
    auto r = NewHoldedMessage(TMessageEx {
        .Src = 1,
        .Dst = 0,
        .Term = term+1,
    }, TRequestVoteRequest {
        .LastLogIndex = 0,
        .LastLogTerm = 0,
        .CandidateId = raft->GetId(),
    });
    messages[0]->Dst = 0;
    ASSERT_NO_FATAL_FAILURE(assert_message_equal(messages[0], *r.Mes));
    messages[1]->Dst = 0;
    ASSERT_NO_FATAL_FAILURE(assert_message_equal(messages[1], *r.Mes));
}

TEST(Raft, candidate_vote_request_small_term) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };

    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 0,
    }, TRequestVoteRequest {
        .LastLogIndex = 0,
        .LastLogTerm = 0,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), std::move(req));
    auto res = NewHoldedMessage(TMessageEx {
        .Src = 1,
        .Dst = 2,
        .Term = raft->GetState()->CurrentTerm,
    }, TRequestVoteResponse {
        .VoteGranted = false,
    });
    ASSERT_EQ(messages.size(), 1);
    ASSERT_NO_FATAL_FAILURE(assert_message_equal(messages[0], *res.Mes));
    ASSERT_EQ(raft->GetState()->CurrentTerm, 1);
}

TEST(Raft, candidate_vote_request_ok_term) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };

    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 1,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), std::move(req));
    auto res = NewHoldedMessage(TMessageEx {
        .Src = 1,
        .Dst = 2,
        .Term = raft->GetState()->CurrentTerm,
    }, TRequestVoteResponse {
        .VoteGranted = true,
    });
    ASSERT_EQ(messages.size(), 1);
    ASSERT_NO_FATAL_FAILURE(assert_message_equal(messages[0], *res.Mes));
    ASSERT_EQ(raft->GetState()->CurrentTerm, 1);
}

TEST(Raft, candidate_vote_request_big) {
    auto raft = MakeRaft();
    auto ts = std::make_shared<TFakeTimeSource>();
    raft->Become(EState::CANDIDATE);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 3,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), req);
    ASSERT_TRUE(raft->CurrentStateName() == EState::FOLLOWER);
}

TEST(Raft, candidate_vote_after_start) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    ASSERT_TRUE(raft->CurrentStateName() == EState::FOLLOWER);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::CANDIDATE);
    raft->ProcessTimeout(ts->Now());
    ASSERT_EQ(raft->GetState()->VotedFor, 1);
    ASSERT_EQ(raft->GetState()->CurrentTerm, 2);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 2,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 2,
    });
    raft->Process(ts->Now(), req);
    auto last = messages.back().Cast<TRequestVoteResponse>();
    ASSERT_EQ(last->VoteGranted, false);

    // request with higher term => follower
    req = NewHoldedMessage(TMessageEx {
        .Src = 3,
        .Dst = 1,
        .Term = 3,
    }, TRequestVoteRequest {
        .LastLogIndex = 1,
        .LastLogTerm = 1,
        .CandidateId = 3,
    });
    raft->Process(ts->Now(), req);
    last = messages.back().Cast<TRequestVoteResponse>();
    ASSERT_EQ(raft->GetState()->VotedFor, 3);
    ASSERT_EQ(last->VoteGranted, true);
}

TEST(Raft, election_5_nodes) {
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft({}, 5);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::CANDIDATE);
    auto req = NewHoldedMessage(TMessageEx {
        .Src = 2,
        .Dst = 1,
        .Term = 2,
    }, TRequestVoteResponse {
        .VoteGranted = true,
    });
    ts->Advance(std::chrono::milliseconds(10000));
    raft->ProcessTimeout(ts->Now());

    raft->Process(ts->Now(), req);

    ASSERT_EQ(raft->CurrentStateName(), EState::CANDIDATE);
    req->Src = 2;
    raft->Process(ts->Now(), req);
    raft->ProcessTimeout(ts->Now());
    ASSERT_EQ(raft->CurrentStateName(), EState::CANDIDATE);

    req->Src = 4;
    raft->Process(ts->Now(), req);
    raft->ProcessTimeout(ts->Now());
    ASSERT_EQ(raft->CurrentStateName(), EState::LEADER);
}

TEST(Raft, commit_advance) {
    auto state = TState {
        /*.CurrentTerm =*/ 1,
        0,
        /*.Log =*/ MakeLog<TLogEntry>({1})
    };

    auto s = TVolatileState {
        .MatchIndex = {{1, 1}}
    };

    auto s1 = TVolatileState(s).CommitAdvance({1, 2, 3}, 3, state);
    ASSERT_EQ(s1.CommitIndex, 1);
    s1 = TVolatileState(s).CommitAdvance({1, 2, 3, 4, 5}, 5, state);
    ASSERT_EQ(s1.CommitIndex, 0);

    s = TVolatileState {
        .MatchIndex = {{1, 1}, {2, 2}}
    };
    auto add = MakeLog<TLogEntry>({1});
    for (auto& entry : add) {
        state.Append(entry);
    }
    s1 = TVolatileState(s).CommitAdvance({1, 2, 3}, 3, state);
    ASSERT_EQ(s1.CommitIndex, 2);
    s1 = TVolatileState(s).CommitAdvance({1, 2, 3, 4, 5}, 5, state);
    ASSERT_EQ(s1.CommitIndex, 1);
}

TEST(Raft, commit_advance_wrong_term) {
    auto state = TState {
        /*.CurrentTerm =*/ 2,
        0,
        /*.Log =*/ MakeLog<TLogEntry>({1,1})
    };
    auto s = TVolatileState {
        .MatchIndex = {{1, 1}, {2, 2}}
    };
    auto s1 = TVolatileState(s).CommitAdvance({1, 2, 3}, 3, state);
    ASSERT_EQ(s1.CommitIndex, 0);
}

TEST(Raft, leader_heartbeat) {
    std::vector<TMessageHolder<TMessage>> messages;
    auto onSend = [&](auto message) {
        messages.emplace_back(std::move(message));
    };
    auto ts = std::make_shared<TFakeTimeSource>();
    auto raft = MakeRaft(onSend, 3);
    ts->Advance(std::chrono::milliseconds(10000));
    raft->Become(EState::LEADER);
    raft->ProcessTimeout(ts->Now());
    ASSERT_EQ(messages.size(), 2);
    auto maybeReq0 = messages[0].Maybe<TAppendEntriesRequest>();
    auto maybeReq1 = messages[1].Maybe<TAppendEntriesRequest>();
    ASSERT_TRUE(maybeReq0);
    ASSERT_TRUE(maybeReq1);
    auto req0 = maybeReq0.Cast();
    auto req1 = maybeReq1.Cast();
    ASSERT_EQ(req0->Src, 1);
    ASSERT_EQ(req1->Src, 1);
    ASSERT_TRUE((req0->Dst == 2 && req1->Dst == 3) || (req0->Dst == 3 && req1->Dst == 2));
    ASSERT_EQ(req0->Nentries, 0);
    ASSERT_EQ(req1->Nentries, 0);
}

TEST(Raft, disk_state) {
    remove("test_disk_state.entries.1");
    remove("test_disk_state.index.1");
    remove("test_disk_state.state.1");

    TDiskState state("test_disk_state", 1);
    ASSERT_EQ(state.LastLogIndex, 0);
    ASSERT_EQ(state.CurrentTerm, 1);
    ASSERT_EQ(state.VotedFor, 0);
    state.CurrentTerm = 6;
    state.Commit();
    auto log = MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6});
    for (auto entry : log) {
        state.Append(entry);
    }
    ASSERT_EQ(state.LastLogIndex, 9);

    std::vector<TMessageHolder<TLogEntry>> log2;
    for (int i = 1; i <= state.LastLogIndex; i++) {
        auto entry = state.Get(i);
        log2.emplace_back(entry);
    }

    assert_terms(log2, {1,1,1,4,4,5,5,6,6});
}

TEST(Raft, disk_state_restore1) {
    remove("test_disk_state_restore1.entries.1");
    remove("test_disk_state_restore1.index.1");
    remove("test_disk_state_restore1.state.1");

    std::unique_ptr<TDiskState> state = std::make_unique<TDiskState>("test_disk_state_restore1", 1);
    ASSERT_EQ(state->LastLogIndex, 0);
    ASSERT_EQ(state->CurrentTerm, 1);
    ASSERT_EQ(state->VotedFor, 0);
    auto log = MakeLog<TLogEntry>({1});
    for (auto entry : log) {
        state->Append(entry);
    }
    ASSERT_EQ(state->LastLogIndex, 1);

    state.reset();
    state = std::make_unique<TDiskState>("test_disk_state_restore1", 1);
    ASSERT_EQ(state->CurrentTerm, 1);
    ASSERT_EQ(state->VotedFor, 0);
    ASSERT_EQ(state->LastLogIndex, 1);

    std::vector<TMessageHolder<TLogEntry>> log2;
    for (int i = 1; i <= state->LastLogIndex; i++) {
        auto entry = state->Get(i);
        log2.emplace_back(entry);
    }

    assert_terms(log2, {1});
}


TEST(Raft, disk_state_restore) {
    remove("test_disk_state_restore.entries.1");
    remove("test_disk_state_restore.index.1");
    remove("test_disk_state_restore.state.1");

    std::unique_ptr<TDiskState> state = std::make_unique<TDiskState>("test_disk_state_restore", 1);
    ASSERT_EQ(state->LastLogIndex, 0);
    ASSERT_EQ(state->CurrentTerm, 1);
    ASSERT_EQ(state->VotedFor, 0);
    // Raft persists a new current term before appending entries from that term.
    state->CurrentTerm = 10;
    state->Commit();
    auto log = MakeLog<TLogEntry>({1,1,1,4,4,5,5,6,6});
    for (auto entry : log) {
        state->Append(entry);
    }
    ASSERT_EQ(state->LastLogIndex, 9);
    state->VotedFor = 2;
    state->Commit();

    state.reset();
    state = std::make_unique<TDiskState>("test_disk_state_restore", 1);
    ASSERT_EQ(state->CurrentTerm, 10);
    ASSERT_EQ(state->VotedFor, 2);
    ASSERT_EQ(state->LastLogIndex, 9);

    std::vector<TMessageHolder<TLogEntry>> log2;
    for (int i = 1; i <= state->LastLogIndex; i++) {
        auto entry = state->Get(i);
        log2.emplace_back(entry);
    }

    assert_terms(log2, {1,1,1,4,4,5,5,6,6});
}

