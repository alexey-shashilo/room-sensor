#include <stdio.h>
#include <math.h>

typedef enum
{
    GAIN_1   = 0U,
    GAIN_2   = 1U,
    GAIN_1_8 = 2U,
    GAIN_1_4 = 3U
} Gain;

typedef enum
{
    IT_25_MS  = 0xCU,
    IT_50_MS  = 0x8U,
    IT_100_MS = 0x0U,
    IT_200_MS = 0x1U,
    IT_400_MS = 0x2U,
    IT_800_MS = 0x3U
} IT;

static double gain_sens[] = { 1.0, 2.0, 0.125, 0.25 };
static double it_ms_value[] = { 25.0, 50.0, 100.0, 200.0, 400.0, 800.0 };

static double it_ms(int it)
{
    switch (it)
    {
        case 0xC: return 25.0;
        case 0x8: return 50.0;
        case 0x0: return 100.0;
        case 0x1: return 200.0;
        case 0x2: return 400.0;
        case 0x3: return 800.0;
        default:  return 0.0;
    }
}

typedef struct
{
    int idx;
    int gain;
    int it;
    double sensitivity;
    double resolution;
} Range;

static const Range ranges[] = {
    { 0, GAIN_1_8, IT_25_MS,   3.125,    1.843200 },
    { 1, GAIN_1_8, IT_50_MS,   6.25,     0.921600 },
    { 2, GAIN_1_4, IT_50_MS,  12.5,      0.460800 },
    { 3, GAIN_1,   IT_25_MS,  25.0,      0.230400 },
    { 4, GAIN_1,   IT_50_MS,  50.0,      0.115200 },
    { 5, GAIN_1,   IT_100_MS, 100.0,     0.057600 },
    { 6, GAIN_1,   IT_200_MS, 200.0,     0.028800 },
    { 7, GAIN_2,   IT_200_MS, 400.0,     0.014400 },
    { 8, GAIN_2,   IT_400_MS, 800.0,     0.007200 },
    { 9, GAIN_2,   IT_800_MS, 1600.0,    0.003600 },
};
static const int num_ranges = sizeof(ranges) / sizeof(ranges[0]);

static int passed = 0;
static int failed = 0;

static double gain_sensitivity(int gain)
{
    if (gain >= 0 && gain < 4) return gain_sens[gain];
    return 0.0;
}

static double compute_sensitivity(int gain, int it)
{
    return gain_sensitivity(gain) * it_ms(it);
}

static double compute_resolution(int gain, int it)
{
    double gs = gain_sensitivity(gain);
    double tim = it_ms(it);
    if (gs <= 0.0 || tim <= 0.0) return 0.0;
    return 0.0576 / gs * (100.0 / tim);
}

static void check(int cond, const char *msg)
{
    if (cond) { passed++; printf("  PASS: %s\n", msg); }
    else      { failed++; printf("  FAIL: %s\n", msg); }
}

int main(void)
{
    printf("VEML7700 Range Table Automated Tests\n\n");

    /* A. Monotonic range table */
    printf("=== A. Monotonic range table ===\n");
    for (int i = 0; i < num_ranges; i++)
    {
        printf("  Range %d: gain=%d it=0x%X sens=%.2f res=%.6f\n",
               ranges[i].idx, ranges[i].gain, ranges[i].it,
               ranges[i].sensitivity, ranges[i].resolution);
    }

    for (int i = 1; i < num_ranges; i++)
    {
        char buf[128];
        sprintf(buf, "ranges[%d].sensitivity (%.2f) > ranges[%d].sensitivity (%.2f)",
                i, ranges[i].sensitivity, i - 1, ranges[i - 1].sensitivity);
        check(ranges[i].sensitivity > ranges[i - 1].sensitivity, buf);
    }

    /* B. Verify sensitivity matches computation */
    printf("\n=== B. Sensitivity formula verification ===\n");
    for (int i = 0; i < num_ranges; i++)
    {
        double expected = ranges[i].sensitivity;
        double actual = compute_sensitivity(ranges[i].gain, ranges[i].it);
        char buf[128];
        sprintf(buf, "range %d: expected sens=%.2f, computed=%.2f", i, expected, actual);
        check(fabs(expected - actual) < 0.001, buf);
    }

    /* C. Resolution verification */
    printf("\n=== C. Resolution verification ===\n");
    for (int i = 0; i < num_ranges; i++)
    {
        double expected = ranges[i].resolution;
        double actual = compute_resolution(ranges[i].gain, ranges[i].it);
        char buf[128];
        sprintf(buf, "range %d: expected res=%.6f, computed=%.6f", i, expected, actual);
        check(fabs(expected - actual) < 0.0001, buf);
    }

    /* D. Range step ratio approximately 2x */
    printf("\n=== D. Step ratio (each step ~2x) ===\n");
    for (int i = 1; i < num_ranges; i++)
    {
        double ratio = ranges[i].sensitivity / ranges[i - 1].sensitivity;
        printf("  %d->%d: %.2fx\n", i - 1, i, ratio);
        char buf[128];
        sprintf(buf, "range %d: ratio %.2f is between 1.5 and 2.5", i, ratio);
        check(ratio >= 1.5 && ratio <= 2.5, buf);
    }

    /* E. Register encoding verification */
    printf("\n=== E. Register encoding ===\n");
    for (int i = 0; i < num_ranges; i++)
    {
        int gain_bits = (int)ranges[i].gain;
        int it_bits = (int)ranges[i].it;
        int conf = (gain_bits << 11) | (it_bits << 6);
        char buf[128];
        sprintf(buf, "range %d: ALS_CONF = 0x%04X", i, conf);
        check(conf > 0, buf);
        printf("         gain=0x%X it=0x%X reg=0x%04X\n", gain_bits, it_bits, conf);
    }

    /* F. Lux per 100, 1000, 10000 counts */
    printf("\n=== F. Lux for typical RAW values ===\n");
    for (int i = 0; i < num_ranges; i++)
    {
        printf("  Range %d: lux/count=%.6f  @100cnt=%.2f  @1000cnt=%.1f  @10000cnt=%.0f\n",
               i, ranges[i].resolution,
               100.0 * ranges[i].resolution,
               1000.0 * ranges[i].resolution,
               10000.0 * ranges[i].resolution);
    }

    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);

    return failed > 0 ? 1 : 0;
}