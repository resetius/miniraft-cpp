#include <miniraft/raft.h>
#include <functional>
#include <gtest/gtest.h>

namespace {
TEST(MessageSafety, typed_allocations_reject_undersized_storage) {
    for (uint32_t size : {0u, 7u, 8u, uint32_t(sizeof(TRequestVoteRequest) - 1)}) {
        for (bool explicitType : {false, true}) {
            bool rejected = false;
            try {
                if (explicitType) NewHoldedMessage<TRequestVoteRequest>(uint32_t(EMessageType::REQUEST_VOTE_REQUEST), size);
                else NewHoldedMessage<TRequestVoteRequest>(size);
            } catch (const std::invalid_argument&) { rejected = true; }
            ASSERT_TRUE(rejected);
        }
    }
}
TEST(MessageSafety, short_wrong_type_and_empty_casts_are_rejected) {
    auto shortFrame = NewHoldedMessage<TMessage>(uint32_t(EMessageType::REQUEST_VOTE_REQUEST), sizeof(TMessage));
    auto wrongType = NewHoldedMessage<TMessage>(uint32_t(EMessageType::LOG_ENTRY), sizeof(TRequestVoteRequest));
    for (auto message : {shortFrame, wrongType, TMessageHolder<TMessage>{}}) {
        ASSERT_FALSE(message.IsEx());
        ASSERT_FALSE(message.Maybe<TRequestVoteRequest>());
        bool rejected = false;
        try { message.Cast<TRequestVoteRequest>(); } catch (const std::bad_cast&) { rejected = true; }
        ASSERT_TRUE(rejected);
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
TEST(MessageSafety, raft_batches_respect_budget_and_continue_replication) {
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
    ASSERT_EQ(peer->Sent.size(), 1);
    auto first = peer->Sent[0];
    ValidateMessageTree(first);
    ASSERT_TRUE(first->Nentries > 0);
    ASSERT_TRUE(first->Nentries < 100);
    ASSERT_EQ(first->Nentries, first.PayloadSize);
    auto reply = NewHoldedMessage(TMessageEx{.Src = 2, .Dst = 1, .Term = 2, .Seqno = first->Seqno},
                                 TAppendEntriesResponse{.MatchIndex = first->Nentries, .Success = 1});
    raft.Process(ITimeSource::Time{}, reply);
    raft.ProcessTimeout(ITimeSource::Time{});
    ASSERT_EQ(peer->Sent.size(), 2);
    auto second = peer->Sent[1];
    ValidateMessageTree(second);
    ASSERT_EQ(second->PrevLogIndex, first->Nentries);
    ASSERT_EQ(first->Nentries + second->Nentries, 100);
    ASSERT_TRUE(second->Nentries > 0);
}
TEST(MessageSafety, invalid_proposal_does_not_mutate_persistent_state) {
    auto state = std::make_shared<TState>();
    TRaft raft(state, 1, {});
    raft.Become(EState::LEADER);
    for (uint32_t length : {0u, uint32_t(sizeof(TLogEntry) - 1), TMessageLimits{}.MaxFrameBytes + 1}) {
        auto entry = NewHoldedMessage<TLogEntry>();
        entry->Len = length;
        entry->Term = 99;
        bool rejected = false;
        try { raft.Append(entry); } catch (const TMessageError&) { rejected = true; }
        ASSERT_TRUE(rejected);
        ASSERT_EQ(state->LastLogIndex, 0);
        ASSERT_EQ(entry->Term, 99);
    }
}
TEST(MessageSafety, unsendable_reply_becomes_error_reply) {
    auto oversized = NewHoldedMessage<TCommandResponse>(sizeof(TCommandResponse) + TMessageLimits{}.MaxFrameBytes);
    oversized->Index = 7;
    oversized->Cookie = 42;
    auto reply = MakeSendable(oversized).Cast<TCommandResponse>();
    ASSERT_EQ(reply->Len, sizeof(TCommandResponse));
    ASSERT_EQ(reply->Index, 7);
    ASSERT_EQ(reply->Cookie, 42);
    ASSERT_EQ(reply->ErrorCode, 2);
    auto valid = NewHoldedMessage<TCommandResponse>();
    ASSERT_TRUE(MakeSendable(valid).Mes == valid.Mes);
    auto request = NewHoldedMessage<TCommandRequest>(sizeof(TCommandRequest) + TMessageLimits{}.MaxFrameBytes);
    ASSERT_FALSE(MakeSendable(request));
}
}
