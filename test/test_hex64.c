#include <stdio.h>
#include <string.h>
#include "hex64.h"

/* Fixed-vector regression for the portable 64-bit lowercase-hex encoder used for
   boot_id. This must produce exactly 16 chars [0-9a-f]{16} independent of libc
   printf long-long support (newlib-nano lacks %ll). */

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static void expect_hex(uint64_t v, const char *expect, const char *name)
{
    char out[17];
    Hex64_ToLower(out, v);
    check(strlen(out) == 16U, name);
    check(strcmp(out, expect) == 0, name);
    int digit_ok = 1;
    for (int i = 0; i < 16; i++)
        if (!((out[i] >= '0' && out[i] <= '9') || (out[i] >= 'a' && out[i] <= 'f')))
            digit_ok = 0;
    check(digit_ok, "chars in [0-9a-f]");
}

int main(void)
{
    printf("Hex64 boot-id vectors\n");
    expect_hex(0x0000000000000000ULL, "0000000000000000", "zero -> 16 zeros");
    expect_hex(0x0000000000000001ULL, "0000000000000001", "one -> ...0001");
    expect_hex(0x0123456789ABCDEFULL, "0123456789abcdef", "pattern lowercase hex");
    expect_hex(0xFFFFFFFFFFFFFFFFULL, "ffffffffffffffff", "all-ones lowercase");

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}