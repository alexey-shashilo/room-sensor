#include <stdio.h>
#include <string.h>

#include "display_pages.h"

/* Display page + gas-index render decision regression (Phase 17.6).

   Covers the deterministic page-switching contract and the SGP41 VOC/NOx
   render policy using ONLY the existing production RoomState validity flags.
   No pixel internals; no SGP41 driver; no gas-index algorithm.

   Negative control: an invalid index MUST NEVER render a numeric value. */

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static void check_str(const char *got, const char *want, const char *name)
{
    check(got != NULL && want != NULL && strcmp(got, want) == 0, name);
}

int main(void)
{
    printf("Display page + gas-index render regressions\n");

    /* ---- A. VOC valid + NOx valid -> numeric values ---- */
    printf("\n--- A. valid indices render numeric ---\n");
    {
        char buf[24];
        DisplayPages_FormatGasLine(buf, sizeof(buf), "VOC", 103,
                                   DISPLAY_GAS_STATE_NUMERIC);
        check_str(buf, "VOC: 103", "VOC valid renders integer");
        DisplayPages_FormatGasLine(buf, sizeof(buf), "NOx", 1,
                                   DISPLAY_GAS_STATE_NUMERIC);
        check_str(buf, "NOx: 1", "NOx valid renders integer");
    }

    /* ---- B. indices invalid during warm-up -> WARM (raw valid present) ---- */
    printf("\n--- B. warm-up (raw valid, index invalid) renders WARM ---\n");
    {
        char buf[24];
        DisplayGasState vs = DisplayPages_GasState(false, true);
        check(vs == DISPLAY_GAS_STATE_WARM, "VOC warm-up -> WARM state");
        DisplayPages_FormatGasLine(buf, sizeof(buf), "VOC", 0, vs);
        check_str(buf, "VOC: WARM", "VOC warm renders WARM");
        DisplayGasState ns = DisplayPages_GasState(false, true);
        DisplayPages_FormatGasLine(buf, sizeof(buf), "NOx", 0, ns);
        check_str(buf, "NOx: WARM", "NOx warm renders WARM");
    }

    /* ---- C. unavailable / failed / stale -> no numeric, render --- ---- */
    printf("\n--- C. unavailable (no raw, no index) renders --- ---\n");
    {
        char buf[24];
        DisplayGasState s = DisplayPages_GasState(false, false);
        check(s == DISPLAY_GAS_STATE_UNAVAILABLE, "no validity -> UNAVAILABLE");
        DisplayPages_FormatGasLine(buf, sizeof(buf), "VOC", 0, s);
        check_str(buf, "VOC: ---", "VOC unavailable renders ---");
        DisplayPages_FormatGasLine(buf, sizeof(buf), "NOx", 999, s);
        check_str(buf, "NOx: ---", "NOx never numeric when unavailable (negative ctrl)");
    }

    /* ---- D. VOC valid / NOx invalid mixed ---- */
    printf("\n--- D. mixed VOC valid / NOx invalid ---\n");
    {
        char buf[24];
        DisplayPages_FormatGasLine(buf, sizeof(buf), "VOC", 88,
                                   DisplayPages_GasState(true, true));
        check_str(buf, "VOC: 88", "VOC numeric, raw present");
        DisplayPages_FormatGasLine(buf, sizeof(buf), "NOx", 42,
                                   DisplayPages_GasState(false, false));
        check_str(buf, "NOx: ---", "NOx non-numeric when index invalid");
    }

    /* ---- E. page switching PAGE1 -> PAGE2 -> PAGE1 ---- */
    printf("\n--- E. page sequence ---\n");
    {
        uint32_t last = 1000u;
        uint8_t p = DISPLAY_PAGE_ENV;
        /* Advance by exactly the period repeatedly. */
        p = DisplayPages_Advance(6000u, &last, p);
        check(p == DISPLAY_PAGE_AIR_QUALITY, "PAGE1 -> PAGE2 at +5s");
        p = DisplayPages_Advance(12000u, &last, p);
        check(p == DISPLAY_PAGE_ENV, "PAGE2 -> PAGE1 at +5s");
    }

    /* ---- F. exact boundary 4999 (no switch) vs 5000 (switch) ---- */
    printf("\n--- F. boundary ---\n");
    {
        uint32_t last = 0u;
        uint8_t p = DISPLAY_PAGE_ENV;
        uint8_t p_4999 = DisplayPages_Advance(4999u, &last, p);
        check(p_4999 == DISPLAY_PAGE_ENV, "4999 ms unchanged");
        /* last_switch_ms stays 0 because no switch occurred yet. */
        uint8_t p_5000 = DisplayPages_Advance(5000u, &last, p);
        check(p_5000 == DISPLAY_PAGE_AIR_QUALITY, "5000 ms switches");
    }

    /* ---- G. uint32 wrap across 0xFFFFFFFF -> 0 ---- */
    printf("\n--- G. wrap ---\n");
    {
        uint32_t last = 0xFFFFFFF0u; /* last switch near top of range */
        uint8_t p = DISPLAY_PAGE_ENV;
        /* 0xFFFFFFFF: elapsed = 0xFFFFFFFF - 0xFFFFFFF0 = 15 -> <5000, no switch */
        uint8_t p1 = DisplayPages_Advance(0xFFFFFFFFu, &last, p);
        check(p1 == DISPLAY_PAGE_ENV, "just below wrap: no switch");
        /* After wrap to 0x00000005: elapsed = 5-0xFFFFFFF0 overflow->21 <5000 */
        uint8_t p2 = DisplayPages_Advance(0x00000005u, &last, p1);
        check(p2 == DISPLAY_PAGE_ENV, "early post-wrap: no switch");
        /* At 0x00002000: elapsed = 0x2000 - 0xFFFFFFF0 (mod 2^32) = 8208 >=5000 */
        uint8_t p3 = DisplayPages_Advance(0x00002000u, &last, p2);
        check(p3 == DISPLAY_PAGE_AIR_QUALITY, "post-wrap +5s+ switches");
    }

    /* ---- H. repeat call before period does not flip ---- */
    printf("\n--- H. idempotent within period ---\n");
    {
        uint32_t last = 100u;
        uint8_t p = DISPLAY_PAGE_ENV;
        uint8_t a = DisplayPages_Advance(3000u, &last, p);
        uint8_t b = DisplayPages_Advance(4000u, &last, a);
        check(a == DISPLAY_PAGE_ENV && b == DISPLAY_PAGE_ENV,
              "no flip within 5s window");
    }

    /* ---- I. buffer truncation safety ---- */
    printf("\n--- I. buffer bounds ---\n");
    {
        char small[6];
        DisplayPages_FormatGasLine(small, sizeof(small), "VOC", 103,
                                   DISPLAY_GAS_STATE_NUMERIC);
        /* snprintf guarantees NUL termination within cap. */
        check(small[sizeof(small) - 1U] == '\0', "short buffer NUL-terminated");
    }

    printf("\nRESULT: %d passed, %d failed\n", s_pass, s_fail);
    if (s_fail > 0) return 1;
    return 0;
}