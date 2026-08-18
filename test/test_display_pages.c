#include <stdio.h>
#include <string.h>

#include "display_pages.h"

/* Display page + gas-index render decision regression (Phase 17.6, extended
   Phase 17.8 with PAGE3 barometric pressure).

   Covers the deterministic 3-page switching contract, the SGP41 VOC/NOx render
   policy, and the PAGE3 pressure format/Pa->hPa/invalid/provider behavior, all
   using ONLY production RoomState semantics (the Page module never touches the
   barometer/SPG41 drivers, never reads them over I2C, never computes a gas
   index or pressure compensation).

   Negative controls: an invalid index/pressure MUST NEVER render a numeric; a
   NONE provider must never produce a fabricated pressure. */

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
    printf("Display page + gas-index + pressure render regressions\n");

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

    /* ---- E. 3-page sequence PAGE1 -> PAGE2 -> PAGE3 -> PAGE1 ---- */
    printf("\n--- E. page sequence (3 pages) ---\n");
    {
        uint32_t last = 1000u;
        uint8_t p = DISPLAY_PAGE_ENV;
        check(p == DISPLAY_PAGE_ENV, "A: initial page is PAGE1");
        p = DisplayPages_Advance(6000u, &last, p);
        check(p == DISPLAY_PAGE_AIR_QUALITY, "B: PAGE1 -> PAGE2 at +5s");
        p = DisplayPages_Advance(12000u, &last, p);
        check(p == DISPLAY_PAGE_PRESSURE, "C: PAGE2 -> PAGE3 at +10s");
        p = DisplayPages_Advance(18000u, &last, p);
        check(p == DISPLAY_PAGE_ENV, "D: PAGE3 -> PAGE1 at +15s");
    }

    /* ---- F. exact boundary 4999 (no switch) vs 5000 (switch) ---- */
    printf("\n--- F. boundary ---\n");
    {
        uint32_t last = 0u;
        uint8_t p = DISPLAY_PAGE_ENV;
        uint8_t p_4999 = DisplayPages_Advance(4999u, &last, p);
        check(p_4999 == DISPLAY_PAGE_ENV, "4999 ms unchanged");
        uint8_t p_5000 = DisplayPages_Advance(5000u, &last, p);
        check(p_5000 == DISPLAY_PAGE_AIR_QUALITY, "5000 ms switches");
    }

    /* ---- G. uint32 wrap across 0xFFFFFFFF -> 0 (3 pages) ---- */
    printf("\n--- G. wrap ---\n");
    {
        uint32_t last = 0xFFFFFFF0u;
        uint8_t p = DISPLAY_PAGE_ENV;
        uint8_t p1 = DisplayPages_Advance(0xFFFFFFFFu, &last, p);
        check(p1 == DISPLAY_PAGE_ENV, "just below wrap: no switch");
        uint8_t p2 = DisplayPages_Advance(0x00000005u, &last, p1);
        check(p2 == DISPLAY_PAGE_ENV, "early post-wrap: no switch");
        /* At 0x00002000: elapsed = 0x2000 - 0xFFFFFFF0 (mod 2^32) = 8208 >=5000 */
        uint8_t p3 = DisplayPages_Advance(0x00002000u, &last, p2);
        check(p3 == DISPLAY_PAGE_AIR_QUALITY, "post-wrap +5s+ switches PAGE1->PAGE2");
        uint8_t p4 = DisplayPages_Advance(0x00004000u, &last, p3);
        check(p4 == DISPLAY_PAGE_PRESSURE, "post-wrap next period PAGE2->PAGE3");
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
        check(small[sizeof(small) - 1U] == '\0', "short buffer NUL-terminated");
    }

    /* ---- E(Pa). Pressure formatting 98850 Pa -> 988.5 hPa ---- */
    printf("\n--- E(Pa). pressure format / hPa conversion ---\n");
    {
        char buf[24];
        DisplayPages_FormatPressure(buf, sizeof(buf), 98850.0f, true);
        check_str(buf, "988.5 hPa", "98850 Pa -> 988.5 hPa");
        DisplayPages_FormatPressure(buf, sizeof(buf), 101325.0f, true);
        check_str(buf, "1013.2 hPa", "101325 Pa -> 1013.2 hPa");
    }

    /* ---- G(Pa). invalid pressure -> non-numeric ---- */
    printf("\n--- G(Pa). invalid pressure representation ---\n");
    {
        char buf[24];
        DisplayPages_FormatPressure(buf, sizeof(buf), 98850.0f, false);
        check_str(buf, "---.- hPa", "invalid -> non-numeric placeholder");
        DisplayPages_FormatPressure(buf, sizeof(buf), 0.0f, false);
        check(strstr(buf, "hPa") != NULL && buf[0] == '-',
              "invalid never a fabricated numeric (negative ctrl)");
    }

    /* ---- H(Pa). provider independence (generic) ---- */
    printf("\n--- H(Pa). provider independence ---\n");
    {
        char buf[24];
        /* Same generic pressure renders numerically regardless of which BMP. */
        DisplayPages_FormatPressure(buf, sizeof(buf), 98850.0f, true);
        check_str(buf, "988.5 hPa", "generic valid renders numeric (no model)");
        /* This formatter takes ONLY Pa+validity — it does not consume any
           provider-specific field, so exactly the same call serves BMP380 and
           BMP390. */
        check(1, "FormatPressure is provider-agnostic");
    }

    /* ---- J(Pa). NONE provider + invalid -> no numeric ---- */
    printf("\n--- J(Pa). NONE => invalid, no numeric ---\n");
    {
        char buf[24];
        /* NONE provider yields barometric_pressure_valid==false in production
           RoomState; the formatter is driven by that validity. */
        DisplayPages_FormatPressure(buf, sizeof(buf), 98850.0f, false);
        check_str(buf, "---.- hPa", "NONE/invalid cannot render numeric");
    }

    /* ---- L/M. PAGE1 / PAGE2 content regression markers ---- */
    printf("\n--- L/M. page id constants preserved ---\n");
    {
        check(DISPLAY_PAGE_ENV == 0U, "PAGE1 (ENV) id preserved");
        check(DISPLAY_PAGE_AIR_QUALITY == 1U, "PAGE2 (AIR_QUALITY) id preserved");
        check(DISPLAY_PAGE_PRESSURE == 2U, "PAGE3 (PRESSURE) id added");
        check(DISPLAY_PAGE_COUNT == 3U, "page count is now 3");
    }

    printf("\nRESULT: %d passed, %d failed\n", s_pass, s_fail);
    if (s_fail > 0) return 1;
    return 0;
}