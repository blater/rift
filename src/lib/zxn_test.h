#ifndef ROCK_ZXN_TEST_H
#define ROCK_ZXN_TEST_H

/* Test-only emulator result protocol. In ordinary builds these functions are
 * no-ops, so the shared Assert.rkr helper remains usable on the host. */
void zxn_test_begin(void);
void zxn_test_stage(const char *stage);
void zxn_test_pass(void);
void zxn_test_fail(void);
void zxn_test_finish(void);

#endif
