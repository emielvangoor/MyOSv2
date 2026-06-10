#pragma once
void tgl_assert_fail(const char *expr);
#define assert(x) ((x) ? (void)0 : tgl_assert_fail(#x))
