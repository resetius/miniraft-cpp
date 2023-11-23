#include <messages.h>

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

void test_empty(void** state) {

}

void test_message_create(void** state) {
    auto mes = NewHoldedMessage<TMessage>(0, 4);
    assert_true(mes->Len == 4);
    assert_true(mes->Type == 0);
}

void test_message_cast(void** state) {
    TMessageHolder<TMessage> mes = NewHoldedMessage<TLogEntry>(static_cast<uint32_t>(TLogEntry::MessageType), 4);
    auto casted = mes.Cast<TLogEntry>();
    assert_true(mes.RawData == casted.RawData);

    auto casted2 = mes.Maybe<RequestVoteRequest>();
    assert_false(casted2);
}

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty),
        cmocka_unit_test(test_message_create),
        cmocka_unit_test(test_message_cast),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
