#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "../src/net.c"

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    // Invariant: Buffer reads never exceed the declared length
    const char *payloads[] = {
        "A",  // Valid input (fits in 10-byte buffer)
        "ABCDEFGHIJ",  // Boundary case (exactly 10 chars, needs 11 bytes with null)
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",  // Oversized by 2.6x
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",  // 100 chars
        "\x41\x41\x41\x41\x41\x41\x41\x41\x41\x41\x41\x41\x41\x41\x41\x41"  // Binary pattern
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        char dest[10] = {0};
        const char *src = payloads[i];
        size_t src_len = strlen(src);
        
        // Test strncpy - should not write beyond dest[9]
        strncpy(dest, src, sizeof(dest));
        
        // Verify no buffer overflow by checking last byte is either null or within bounds
        ck_assert_msg(dest[sizeof(dest)-1] == '\0' || src_len < sizeof(dest),
                     "Buffer overflow detected: strncpy didn't null-terminate when src_len >= %zu",
                     sizeof(dest));
        
        // Additional safety check: ensure we can read entire buffer without crash
        for (size_t j = 0; j < sizeof(dest); j++) {
            volatile char c = dest[j];  // Force read
            (void)c;
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_reads_never_exceed_declared_length);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}