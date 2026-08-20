#include "display_pages.h"
#include <stdio.h>
#include <math.h>

/* mmHg <- Pa PRESENTATION-ONLY conversion (Phase 17.8A). Canonical internal
   unit stays Pa (RoomState + telemetry); only the PAGE3 display string uses
   mmHg. Exact physical definition: 1 mmHg = 133.322387415 Pa, i.e.
   mmHg = Pa / 133.322387415. */
#define PA_PER_MMHG 133.322387415

uint8_t DisplayPages_Advance(uint32_t now, uint32_t *last_switch_ms, uint8_t current_page)
{
    if (last_switch_ms == NULL)
        return DISPLAY_PAGE_ENV;

    /* Wrap-safe elapsed check: modular subtraction never overflows and yields
       the true elapsed ticks across the 0xFFFFFFFF -> 0 boundary. */
    uint32_t elapsed = (uint32_t)(now - *last_switch_ms);
    if (elapsed >= DISPLAY_PAGE_PERIOD_MS)
    {
        *last_switch_ms += DISPLAY_PAGE_PERIOD_MS;
        /* PAGE1 -> PAGE2 -> PAGE3 -> PAGE1 cycle. */
        current_page = (uint8_t)(current_page + 1U);
        if (current_page >= DISPLAY_PAGE_COUNT)
            current_page = DISPLAY_PAGE_ENV;
    }

    if (current_page >= DISPLAY_PAGE_COUNT)
        current_page = DISPLAY_PAGE_ENV;

    return current_page;
}

DisplayGasState DisplayPages_GasState(bool index_valid, bool raw_valid)
{
    if (index_valid)
        return DISPLAY_GAS_STATE_NUMERIC;

    /* Production semantics: during warm-up the SGP41 runtime produces a fresh
       RAW sample but the gas-index is not yet out of blackout, so raw_valid is
       true while index_valid is false. That exact existing signature is what
       renders "WARM". Any unavailable/failed/stale sensor clears BOTH flags. */
    if (raw_valid)
        return DISPLAY_GAS_STATE_WARM;

    return DISPLAY_GAS_STATE_UNAVAILABLE;
}

void DisplayPages_FormatGasLine(char *buf, size_t cap, const char *label,
                                int32_t value, DisplayGasState state)
{
    if (buf == NULL || cap == 0U || label == NULL)
        return;

    switch (state)
    {
        case DISPLAY_GAS_STATE_NUMERIC:
            (void)snprintf(buf, cap, "%s: %ld", label, (long)value);
            break;
        case DISPLAY_GAS_STATE_WARM:
            (void)snprintf(buf, cap, "%s: WARM", label);
            break;
        case DISPLAY_GAS_STATE_UNAVAILABLE:
        default:
            (void)snprintf(buf, cap, "%s: ---", label);
            break;
    }
}

/* PAGE3 pressure formatter. Pa -> mmHg (Pa / 133.322387415) for PRESENTATION
   only; the generic RoomState stays in Pa. Rounds to the nearest whole mmHg
   ("742 mmHg") for readability on the small SSD1306 — no decimal needed for
   ordinary atmospheric pressure. Invalid/provider-NONE/non-finite never renders
   a numeric, so no fabricated pressure is shown. Provider-agnostic: it consumes
   only the generic Pa + validity, identical for BMP380 and BMP390. */
void DisplayPages_FormatPressure(char *buf, size_t cap, float pressure_pa, bool valid)
{
    if (buf == NULL || cap == 0U)
        return;

    if (!valid || !isfinite((double)pressure_pa))   /* non-finite also invalid */
    {
        (void)snprintf(buf, cap, "--- mmHg");
        return;
    }

    (void)snprintf(buf, cap, "%.0f mmHg",
                   (double)(pressure_pa / (float)PA_PER_MMHG));
}
