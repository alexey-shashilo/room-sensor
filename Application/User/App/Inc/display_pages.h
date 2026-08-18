#ifndef DISPLAY_PAGES_H
#define DISPLAY_PAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Display page contract (Phase 17.6): App alternates two pages on the SSD1306.
   This module owns ONLY the page-selection and gas-index render DECISIONS so
   they are deterministic and host-testable without pixel internals. App owns the
   actual rendering via the existing Display_ abstraction. No SGP41 driver or
   gas-index algorithm is used here. */

#define DISPLAY_PAGE_ENV            0U  /* PAGE 1: existing environmental page */
#define DISPLAY_PAGE_AIR_QUALITY    1U  /* PAGE 2: SGP41 VOC/NOx air-quality page */
#define DISPLAY_PAGE_COUNT          2U

/* Automatic PAGE1 <-> PAGE2 alternation period. */
#define DISPLAY_PAGE_PERIOD_MS      5000U

/* Render state for ONE gas-index channel, derived from the existing production
   validity flags. This is NOT a second warm-up state machine: it interprets the
   RoomState vality semantics the SGP41 runtime already produces. */
typedef enum
{
    DISPLAY_GAS_STATE_UNAVAILABLE = 0,  /* no fresh raw and no valid index -> "---" */
    DISPLAY_GAS_STATE_WARM,             /* fresh raw collected, index not yet out of blackout -> "WARM" */
    DISPLAY_GAS_STATE_NUMERIC           /* production index validity true -> render integer */
} DisplayGasState;

#ifdef __cplusplus
extern "C" {
#endif

/* Wrap-safe page advance. Toggles *page between DISPLAY_PAGE_ENV and
   DISPLAY_PAGE_AIR_QUALITY every DISPLAY_PAGE_PERIOD_MS, tracked via the
   elapsed-tick marker *last_switch_ms. Non-blocking (no sleep/wait call);
   survives uint32 tick wrap because it uses modular-arithmetic subtraction.
   Returns the active page (0 or 1). */
uint8_t DisplayPages_Advance(uint32_t now, uint32_t *last_switch_ms, uint8_t current_page);

/* Map the production raw-valid / index-valid flags to a render decision for
   one gas channel. index_valid is the authoritative "may render numeric" gate. */
DisplayGasState DisplayPages_GasState(bool index_valid, bool raw_valid);

/* Format one gas line per DISPLAY_GAS_STATE:
     NUMERIC    -> "<label>: <value>"   (e.g. "VOC: 103")
     WARM       -> "<label>: WARM"
     UNAVAILABLE-> "<label>: ---"
   Never emits a numeric unless state == DISPLAY_GAS_STATE_NUMERIC. */
void DisplayPages_FormatGasLine(char *buf, size_t cap, const char *label,
                                int32_t value, DisplayGasState state);

#ifdef __cplusplus
}
#endif

#endif