#ifndef RECOVERY_POLICY_H
#define RECOVERY_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

/* ================================================================
   Bounded automatic sensor recovery policy (portable).
   ================================================================

   Centralizes the classification and bounded-backoff policy shared by the
   sensor runtimes and the shared-bus health monitor so the pieces stay
   consistent and are directly regression-testable.

   Failure classes (Phase 2):
      NOT_FOUND    device physically absent / no ACK on probe.
      BUS_ERROR    transport-level failure (NACK/AF, HAL BUSY, bus error).
      TIMEOUT      transport-level operation exceeded its time budget.
      DEVICE_ERROR device communicated but reported an internal fault
                   (BMP390 ERR register fatal/cmd/conf). Device-local.
      ERROR/CRC    data-integrity failure (Sensirion CRC-8 mismatch).
      STALE        a previously accepted sample exceeded its freshness window.

   Classification (A=device-local, B=transport/bus-level, C=data-integrity,
   D=device physically absent):
      NOT_FOUND     -> D
      BUS_ERROR     -> B (but only counts toward shared-bus recovery when it
                         comes from a PREVIOUSLY-HEALTHY device)
      TIMEOUT       -> B (same caveat as BUS_ERROR)
      DEVICE_ERROR  -> A
      CRC_ERROR     -> C
      STALE         -> A/C (freshness policy)
      VERIFY_ERROR  -> C

   Bounded re-probe backoff for physically absent (NOT_FOUND) devices: an absent
   optional sensor must not be probed on every App_DoRetry cycle (5 s) forever.
   Backoff ladder (per device, NOT global): 5s -> 10s -> 30s -> 60s capped at
   60s. A successful probe/live transaction resets the ladder to 5s.

   Shared-bus recovery evidence:
      A bus-level recovery may be considered ONLY when EITHER
        (a) >= 2 previously-present/healthy devices report BUS_ERROR/TIMEOUT
            within a bounded observation window; OR
        (b) the bus reports a persistent BUSY/error state after bounded retries
            (driven by a bus-level probe returning BUS_ERROR repeatedly).
      NOT_FOUND from a never-present device, CRC/data errors, and BMP390
      DEVICE_ERROR NEVER count as shared-bus evidence. */

/* Per-device NOT_FOUND re-probe backoff. */
#define RECOVERY_BACKOFF_LEVEL0_MS  5000U
#define RECOVERY_BACKOFF_LEVEL1_MS  10000U
#define RECOVERY_BACKOFF_LEVEL2_MS  30000U
#define RECOVERY_BACKOFF_LEVEL3_MS  60000U

/* Shared-bus health observation window and evidence threshold. */
#define RECOVERY_BUS_WINDOW_MS      10000U
#define RECOVERY_BUS_EVIDENCE_MIN   2U

/* Minimum interval between shared-bus recovery attempts (bus-recovery cooldown,
   Phase 2). Even with persistent multi-device transport evidence, a second
   I2cBus_Recover cannot be issued until at least this much has elapsed since the
   previous attempt. Together with the 5 s App retry cadence, this caps recovery
   frequency and prevents a tight recovery loop when the bus is persistently bad:
   each recovery attempt has time for the runtimes to re-probe and report before
   the next attempt is eligible.

   Justification for 60 s: a physically present but pathological bus that fails
   to re-init is rare; a 1-minute minimum between attempts bounds the worst-case
   recovery rate while giving the affected runtimes a full NOT_FOUND/sensor
   recovery window (SCD41 first-sample ~5 s, SHT45/BMP390 sub-second) to heal
   WITHOUT a further bus reinit. It also keeps worst-case synchronous App work
   negligible (each attempt is a single bounded DeInit/Init pair). This matches
   the documented escalation sensor recovery -> bus recovery, where the bus is
   the LAST resort and should not dominate the loop. */
#define RECOVERY_BUS_COOLDOWN_MS    60000U

typedef enum
{
    FAIL_CLASS_DEVICE_LOCAL = 0,  /* A: device-local fault */
    FAIL_CLASS_TRANSPORT,         /* B: bus/transport-level */
    FAIL_CLASS_DATA,              /* C: data-integrity */
    FAIL_CLASS_ABSENT             /* D: device physically absent */
} RecoveryFailClass;

/* Classify a DriverStatus into the portable failure class (see header). */
static inline RecoveryFailClass RecoveryPolicy_Classify(DriverStatus status)
{
    switch (status)
    {
        case DRIVER_STATUS_NOT_FOUND: return FAIL_CLASS_ABSENT;
        case DRIVER_STATUS_BUS_ERROR:
        case DRIVER_STATUS_TIMEOUT:    return FAIL_CLASS_TRANSPORT;
        case DRIVER_STATUS_CRC_ERROR:
        case DRIVER_STATUS_VERIFY_ERROR: return FAIL_CLASS_DATA;
        case DRIVER_STATUS_DEVICE_ERROR: return FAIL_CLASS_DEVICE_LOCAL;
        case DRIVER_STATUS_NOT_READY:
        default:                        return FAIL_CLASS_DEVICE_LOCAL;
    }
}

/* NOT_FOUND re-probe backoff ladder. `consecutive_absent` is the number of
   consecutive NOT_FOUND samples observed for this device; returns the delay in
   ms before the next re-probe. Clamped to the cap. */
static inline uint32_t RecoveryPolicy_BackoffMs(uint32_t consecutive_absent)
{
    if (consecutive_absent >= 3U)
        return RECOVERY_BACKOFF_LEVEL3_MS;
    if (consecutive_absent == 2U)
        return RECOVERY_BACKOFF_LEVEL2_MS;
    if (consecutive_absent == 1U)
        return RECOVERY_BACKOFF_LEVEL1_MS;
    return RECOVERY_BACKOFF_LEVEL0_MS;
}

/* Advance / reset a per-device absence counter based on the latest sample.
   Returns the updated consecutive-absent count. A live (non-absent) result
   resets to 0. */
static inline uint32_t RecoveryPolicy_TrackAbsence(uint32_t consecutive_absent, bool absent)
{
    return absent ? (consecutive_absent + 1U) : 0U;
}

/* Wrap-safe elapsed check: true when now is at-or-after since+window_ms. */
static inline bool RecoveryPolicy_Elapsed(uint32_t now, uint32_t since, uint32_t window_ms)
{
    uint32_t delta = (uint32_t)(now - since);
    return (delta < 0x80000000U) && (delta >= window_ms);
}

/* Wrap-safe "inside window" check: true when the tick `t` falls within
   [now - window_ms, now]. Used to decide whether a recorded transport failure
   is still recent enough to count as shared-bus evidence. */
static inline bool RecoveryPolicy_WindowWithin(uint32_t now, uint32_t t, uint32_t window_ms)
{
    uint32_t delta = (uint32_t)(now - t);
    return (delta < 0x80000000U) && (delta < window_ms);
}

#endif