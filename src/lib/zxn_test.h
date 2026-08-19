#ifndef RIFT_ZXN_TEST_H
#define RIFT_ZXN_TEST_H

#include "typedefs.h"

/* Test-only emulator result protocol. In ordinary builds these functions are
 * no-ops, so the shared Assert.rift helper remains usable on the host. */
void zxn_test_begin(void);
void zxn_test_stage(const char *stage);
void zxn_test_stage_string(string stage);
void zxn_test_pass(void);
void zxn_test_fail(void);
void zxn_test_assert_pass(string description);
void zxn_test_assert_fail(string description, string expected, string actual);
void zxn_test_finish(void);

#endif
