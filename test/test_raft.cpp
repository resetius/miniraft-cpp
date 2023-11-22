#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

void test_empty(void** state) {

}

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty),
    };
    return 0;
}
