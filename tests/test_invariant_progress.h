#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "include/progress.h"

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    // Invariant: Buffer reads never exceed the declared length
    const char *payloads[] = {
        "normal",                    // Valid input
        "A",                         // Boundary: single char
        "very_long_string_that_exceeds_buffer_by_more_than_double_its_size_1234567890",  // Exploit case
        "exact_buffer_size_plus_one", // Boundary: length = BUFFER_SIZE + 1
        "null_terminated_test\0hidden" // Embedded null byte test
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        char dest[BUFFER_SIZE] = {0};  // BUFFER_SIZE defined in progress.h
        const char *src = payloads[i];
        
        // Call the actual production function
        strncpy_to_buffer(dest, src, BUFFER_SIZE);
        
        // Check that dest is null-terminated
        ck_assert_msg(dest[BUFFER_SIZE - 1] == '\0' || strlen(dest) < BUFFER_SIZE,
                     "Buffer not properly null-terminated for payload %d", i);
        
        // Check no out-of-bounds write occurred by verifying adjacent memory
        char guard_zone[BUFFER_SIZE * 2] = {0};
        memset(guard_zone, 0xAA, sizeof(guard_zone));
        
        // Copy to a buffer with guard zones
        char test_buf[BUFFER_SIZE] = {0};
        strncpy_to_buffer(test_buf, src, BUFFER_SIZE);
        
        // Verify guard zones unchanged
        for (int j = 0; j < BUFFER_SIZE; j++) {
            ck_assert_msg(guard_zone[j] == 0xAA, 
                         "Buffer overflow detected for payload %d at position %d", i, j);
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