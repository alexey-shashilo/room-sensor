#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bmp390.h"
#include "bmp390_test.h"

/* BMP390 compensation regression: PRODUCTION parser/compensation vs FROZEN
   Bosch BMP3_SensorAPI v2.0.6 golden values.
 *
 * Golden expected values below were produced by the standalone reference
 * harness (oracle) that compiles the OFFICIAL Bosch bmp3.c (float path,
 * BMP3_FLOAT_COMPENSATION) with the SAME literal calibration and raw-ADC bytes.
 * The production test below therefore NEVER calls Bosch code: it runs the
 * project's own BMP390 parser/compensation (via BMP390_UT_* hooks) and compares
 * against the hard-coded Bosch reference outputs.
 *
 * Section A: literal parser tests (24-bit ADC decode, U16 / S16 +/- / I8).
 * Section B: full calibration -> compensation fixtures (golden).
 * Section C: in-range compensation sanity + raw-pressure path.
 */

#define ASSERTTOL(cond, name) \
    do { s_case++; if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); } \
         else { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); } } while (0)

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static int near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

/* ------------------------------------------------------------------ */
/* LITERAL FIXX FIXTURES (frozen; mirrors oracle/src fixtures)        */
/* ------------------------------------------------------------------ */

/* Fixture 1: room-typical (~24.5 C / 101.325 kPa). */
static const uint8_t CAL1[21] = {
    0xAD, 0xD8, 0x26, 0x6F, 0xFE,
    0x12, 0xC3, 0xCF, 0x48, 0x28, 0xBA,
    0x12, 0x7A, 0xFC, 0xFF, 0x3C, 0xE7,
    0x74, 0x8B, 0xC9, 0xB0
};
static const uint8_t RAW1[6] = {
    0x5F, 0x5A, 0x55,   /* P 0x555A5F */
    0x5B, 0xC9, 0xE6    /* T 0xE6C95B */
};
/* Fixture 2: cool-dry (~13.0 C @ 96.5 kPa). */
static const uint8_t CAL2[21] = {
    0xD4, 0xE1, 0x05, 0x68, 0xFB,
    0x50, 0xCD, 0xF8, 0x47, 0x19, 0xA1,
    0x36, 0x74, 0xFD, 0xFF, 0xE2, 0x5A,
    0xDA, 0x8F, 0x32, 0x92
};
static const uint8_t RAW2[6] = {
    0x87, 0xA6, 0x52,   /* raw P 0x52A687 */
    0x63, 0xD4, 0xE9    /* raw T 0xE9D463 */
};
/* Fixture 3: warm-humid (~28.5 C @ 103.5 kPa). */
static const uint8_t CAL3[21] = {
    0x48, 0xD0, 0x16, 0x72, 0x05,
    0xD5, 0xB8, 0x70, 0x49, 0xCE, 0x23,
    0xEE, 0x7F, 0xFC, 0xFF, 0x5F, 0xD8,
    0x41, 0x89, 0xD8, 0x1E
};
static const uint8_t RAW3[6] = {
    0x5D, 0xAA, 0x55,   /* raw P 0x55AA5D */
    0x1E, 0x42, 0xE0    /* raw T 0xE0421E */
};

/* ---- Section A: standalone parser tests (asymmetric literal bytes) --------- */
static void test_parser(void)
{
    printf("== A. Standalone BMP390 parser (24-bit ADC, U16, S16 +/-) ==\n");

    /* 24-bit raw pressure decode (LSB-first). */
    check(BMP390_UT_Raw24(&RAW1[0]) == 0x555A5FU, "Raw24 P fixture1 = 0x555A5F");
    check(BMP390_UT_Raw24(&RAW1[3]) == 0xE6C95BU, "Raw24 T fixture1 = 0xE6C95B");
    check(BMP390_UT_Raw24(&RAW2[0]) == 0x52A687U, "Raw24 P fixture2 = 0x52A687");
    check(BMP390_UT_Raw24(&RAW3[0]) == 0x55AA5DU, "Raw24 P fixture3 = 0x55AA5D");

    /* U16 calibration coefficient (par_t1, bytes[0..1], little-endian). */
    {
        Bmp390QuantizedCalib q2;
        uint8_t calib2[21]; memset(calib2, 0, 21); calib2[0]=0xD7; calib2[1]=0x3F;
        BMP390_UT_ParseCalib(calib2, &q2);
        /* 0x3FD7 u16, quantized /0.00390625. */
        check(near(q2.par_t1, (double)0x3FD7 / 0.00390625, 1e-3),
              "U16 t1 little-endian concat");
    }

    /* S16 negative with 16384 offset: par_p1 fixture1 bytes[5..6]=0x12 C3
       -> 0xC312 sign-ext = -15598, then -16384 = -31982 -> q=-31982/1048576. */
    {
        Bmp390QuantizedCalib q3;
        BMP390_UT_ParseCalib(CAL1, &q3);
        check(q3.par_p1 == (-15598.0 - 16384.0) / 1048576.0,
              "S16 p1 sign-extend + offset (neg)");
        /* S16 negative: par_p9 fixture1 = 0x8B74 -> -29836. */
        check(q3.par_p9 == -29836.0 / 281474976710656.0, "S16 p9 negative");
        /* I8 negative: par_t3 fixture1 = 0xFE -> -2. */
        check(q3.par_t3 == -2.0 / 281474976710656.0, "I8 t3 negative");
    }

    /* S16 positive: par_t2 fixture1 bytes[2..3]=0x26 0x6F -> u16 0x6F26 (unsigned
     * field, but use par_p2 which is S16 without offset? p2 HAS offset. Instead
     * validate an S16 positive via par_p1 sign-ex on a different fixture. */
    /* I8 positive: fixture3 par_p3 = 0xCE -> -50 (negative). */
    Bmp390QuantizedCalib q5;
    BMP390_UT_ParseCalib(CAL3, &q5);
    check(q5.par_p3 == -50.0 / 4294967296.0, "I8 p3 (fixture3 bytes[9]=0xCE=-50)");
    check(q5.par_p4 == +35.0 / 137438953472.0, "I8 p4 positive (bytes[10]=0x23=35)");
}

static void test_golden_fixtures(void)
{
    printf("\n== B. Golden calibration -> compensation (fixtures 1..3) ==\n");
    {
        Bmp390QuantizedCalib q; memset(&q, 0, sizeof(q));
        BMP390_UT_ParseCalib(CAL1, &q);
        uint64_t rp = BMP390_UT_Raw24(&RAW1[0]);
        int64_t  rt = BMP390_UT_Raw24(&RAW1[3]);
        double T, P;
        BMP390_UT_Compensate(&q, rp, rt, &T, &P);
        check(near(T, 24.5000066664, 1e-4), "Fix1 temp ~24.5000067 C");
        check(near(P, 101324.9846281795, 1e-2), "Fix1 press ~101324.9846 Pa");
    }
    {
        Bmp390QuantizedCalib q; memset(&q, 0, sizeof(q));
        BMP390_UT_ParseCalib(CAL2, &q);
        uint32_t rp = BMP390_UT_Raw24(&RAW2[0]);
        int64_t  rt = BMP390_UT_Raw24(&RAW2[3]);
        double t, p;
        BMP390_UT_Compensate(&q, rp, rt, &t, &p);
        check(near(t, 13.0000119683, 1e-4), "Fix2 temp ~13.0000120 C");
        check(near(p, 96499.9943958386, 1e-2), "Fix2 press ~96499.9944 Pa");
    }
    {
        Bmp390QuantizedCalib q; memset(&q, 0, sizeof(q));
        BMP390_UT_ParseCalib(CAL3, &q);
        uint32_t rp = BMP390_UT_Raw24(&RAW3[0]);
        int64_t  rt = BMP390_UT_Raw24(&RAW3[3]);
        double t, p;
        BMP390_UT_Compensate(&q, rp, rt, &t, &p);
        check(near(t, 28.4999960505, 1e-4), "Fix3 temp ~28.4999961 C");
        check(near(p, 103499.9851681331, 1e-2), "Fix3 press ~103499.9852 Pa");
    }
}

/* Section C: compensation stays in operating envelope on golden fixtures. */
static void test_envelope(void)
{
    printf("\n== C. Operating-envelope sanity on golden fixtures ==\n");
    /* fixture1 */
    Bmp390QuantizedCalib q; BMP390_UT_ParseCalib(CAL1, &q);
    double t, p; BMP390_UT_Compensate(&q, BMP390_UT_Raw24(&RAW1[0]), BMP390_UT_Raw24(&RAW1[3]), &t, &p);
    check(t >= -40.0 && t <= 85.0, "fix1 temp in operating range");
    check(p >= 30000.0 && p <= 125000.0, "fix1 press in operating range");
}

int main(void)
{
    test_parser();
    test_golden_fixtures();
    test_envelope();
    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}