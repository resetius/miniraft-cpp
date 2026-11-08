#include <miniraft/raft.h>
#include <functional>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

namespace {
void test_typed_allocations_reject_undersized_storage(void**) {
    for (uint32_t size : {0u, 7u, 8u, uint32_t(sizeof(TRequestVoteRequest) - 1)}) {
        for (bool explicitType : {false, true}) {
            bool rejected = false;
            try {
                if (explicitType) NewHoldedMessage<TRequestVoteRequest>(uint32_t(EMessageType::REQUEST_VOTE_REQUEST), size);
                else NewHoldedMessage<TRequestVoteRequest>(size);
            } catch (const std::invalid_argument&) { rejected = true; }
            assert_true(rejected);
        }
    }
}
void test_short_wrong_type_and_empty_casts_are_rejected(void**) {
    auto shortFrame = NewHoldedMessage<TMessage>(uint32_t(EMessageType::REQUEST_VOTE_REQUEST), sizeof(TMessage));
    auto wrongType = NewHoldedMessage<TMessage>(uint32_t(EMessageType::LOG_ENTRY), sizeof(TRequestVoteRequest));
    for (auto message : {shortFrame, wrongType, TMessageHolder<TMessage>{}}) {
        assert_false(message.IsEx());
        assert_false(message.Maybe<TRequestVoteRequest>());
        bool rejected = false;
        try { message.Cast<TRequestVoteRequest>(); } catch (const std::bad_cast&) { rejected = true; }
        assert_true(rejected);
    }
}
struct TEndpoint : INode {
    std::vector<TMessageHolder<TAppendEntriesRequest>> Sent;
    void Send(TMessageHolder<TMessage> message) override {
        if (message->Type == uint32_t(EMessageType::APPEND_ENTRIES_REQUEST))
            Sent.push_back(message.Cast<TAppendEntriesRequest>());
    }
    void Drain() override {}
};
void test_raft_batches_respect_budget_and_continue_replication(void**) {
    auto state = std::make_shared<TState>();
    state->CurrentTerm = 2;
    auto entry = NewHoldedMessage<TLogEntry>(1024 * 1024);
    entry->Term = 2;
    for (int i = 0; i < 100; ++i) state->Append(entry);
    auto peer = std::make_shared<TEndpoint>();
    TRaft raft(state, 1, {{2, peer}});
    raft.Become(EState::LEADER);
    auto* progress = const_cast<TVolatileState*>(raft.GetVolatileState());
    progress->BatchSize[2] = 1024;
    raft.ProcessTimeout(ITimeSource::Time{});
    assert_int_equal(peer->Sent.size(), 1);
    auto first = peer->Sent[0];
    ValidateMessageTree(first);
    assert_true(first->Nentries > 0);
    assert_true(first->Nentries < 100);
    assert_int_equal(first->Nentries, first.PayloadSize);
    auto reply = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 2, .Seqno = first->Seqno},
                                 TAppendEntriesResponse{.MatchIndex = first->Nentries, .Success = 1});
    raft.Process(ITimeSource::Time{}, reply);
    raft.ProcessTimeout(ITimeSource::Time{});
    assert_int_equal(peer->Sent.size(), 2);
    auto second = peer->Sent[1];
    ValidateMessageTree(second);
    assert_int_equal(second->PrevLogIndex, first->Nentries);
    assert_int_equal(first->Nentries + second->Nentries, 100);
    assert_true(second->Nentries > 0);
}
void test_invalid_proposal_does_not_mutate_persistent_state(void**) {
    auto state = std::make_shared<TState>();
    TRaft raft(state, 1, {});
    raft.Become(EState::LEADER);
    for (uint32_t length : {0u, uint32_t(sizeof(TLogEntry) - 1), TMessageLimits{}.MaxFrameBytes + 1}) {
        auto entry = NewHoldedMessage<TLogEntry>();
        entry->Len = length;
        entry->Term = 99;
        bool rejected = false;
        try { raft.Append(entry); } catch (const TMessageError&) { rejected = true; }
        assert_true(rejected);
        assert_int_equal(state->LastLogIndex, 0);
        assert_int_equal(entry->Term, 99);
    }
}
}
int main() {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_typed_allocations_reject_undersized_storage),
        cmocka_unit_test(test_short_wrong_type_and_empty_casts_are_rejected),
        cmocka_unit_test(test_raft_batches_respect_budget_and_continue_replication),
        cmocka_unit_test(test_invalid_proposal_does_not_mutate_persistent_state),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
