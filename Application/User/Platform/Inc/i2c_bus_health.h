#ifndef I2C_BUS_HEALTH_H
#define I2C_BUS_HEALTH_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"
#include "room_sensor_types.h"
#include "recovery_policy.h"

/* ================================================================
   Shared I2C bus health monitor (portable).
   ================================================================

   Decides whether a genuine shared-bus failure exists and drives bounded
   shared-bus recovery. It is conservative by design: a single device failure
   must NOT reinitialize the bus.

   Evidence model (wrap-safe, time-rolling window):
     - Each previously-healthy device slot records the tick of its most recent
       transport-level failure (BUS_ERROR/TIMEOUT) in last_transport_ms[].
     - The SAME window_open_ms anchors the observation epoch. On every Report,
       if the window has aged >= RECOVERY_BUS_WINDOW_MS the epoch is expired and
       a fresh one begins, so stale evidence can never participate in the next
       decision.
     - transport_evidence re-counts the DISTINCT previously-healthy slots whose
       last transport failure falls inside the CURRENT window. So:
         A BUS_ERROR x100          -> 1 distinct slot -> never enough;
         A BUS_ERROR + A TIMEOUT   -> 1 distinct slot -> never enough;
         A+B in-window             -> 2 -> recovery eligible;
         A+B outside the window    -> window expired -> not eligible.

   Never counts toward bus evidence:
     - NOT_FOUND from a device that was never present (absent optional sensor);
     - CRC/data-integrity errors;
     - BMP390 DEVICE_ERROR (a device-level fault register, not a bus problem);

   Cooldown (storm protection):
     - RECOVERY_BUS_COOLDOWN_MS is the minimum interval between recovery
       attempts. Even with persistent multi-device evidence, a second attempt is
       not eligible until the cooldown has elapsed, so shared-bus recovery can
       NEVER execute in a tight loop.

   Recovery attempt lifecycle:
     - I2cBusHealth_BeginRecovery() MUST be called immediately before I2cBus_Recover.
       It increments bus_recovery_attempts, records the cooldown timestamp, and
       CLEARS the evidence epoch so OLD evidence can never participate again —
       regardless of whether the attempt then succeeds or fails. This happens
       once, up-front, satisfying "clear evidence after a recovery attempt,
       whether it succeeds or fails".
     - I2cBusHealth_OnRecoverySuccess() / I2cBusHealth_OnRecoveryFailure()
       complete the attempt and keep the invariant:
            bus_recovery_attempts == successes + failures.
       bus_recovery_count (legacy helper) == successes.

   Notable: the monitor never performs a blocking loop or spin; recovery is
   executed by the caller. After a successful recovery, sensors are NOT auto-
   marked valid — each must obtain a fresh sample. Header-inline so the same
   portable source is available to every firmware target and host test. */

typedef enum
{
    I2C_BUS_HEALTH_OK = 0,
    I2C_BUS_HEALTH_RECOVERING
} I2cBusHealthState;

typedef struct
{
    /* Distinct previously-healthy device slots that reported a transport
       failure within the current window. */
    uint32_t transport_evidence;
    /* Per-slot last transport-failure tick (0 = never failed transport-level). */
    uint32_t last_transport_ms[8];
    /* Anchor of the current observation epoch (tick at which the window opened). */
    uint32_t window_open_ms;
    bool     window_open;
    /* Device is known to be absent it never responded / not ever present. */
    bool     never_present[8];
    /* Device is positively established as previously present/healthy. */
    bool     previously_healthy[8];

    /* Recovery attempt lifecycle + diagnostics (Phase 5). */
    uint32_t bus_recovery_attempts;
    uint32_t bus_recovery_successes;
    uint32_t bus_recovery_failures;
    uint32_t last_recovery_ms;      /* tick of the last BEGIN (cooldown base) */
    bool     ever_recovered;        /* distinguishes "never" from a real tick */

    I2cBusHealthState state;
} I2cBusHealth;

/* Reset the monitor for a fresh boot. */
static inline void I2cBusHealth_Init(I2cBusHealth *h)
{
    if (h == NULL) return;
    {
        int i;
        for (i = 0; i < 8; i++)
        {
            h->last_transport_ms[i] = 0U;
            h->never_present[i] = true;
            h->previously_healthy[i] = false;
        }
    }
    h->transport_evidence = 0U;
    h->window_open_ms = 0U;
    h->window_open = false;
    h->bus_recovery_attempts = 0U;
    h->bus_recovery_successes = 0U;
    h->bus_recovery_failures = 0U;
    h->last_recovery_ms = 0U;
    h->ever_recovered = false;
    h->state = I2C_BUS_HEALTH_OK;
}

/* Register current-discovery presence. IMPORTANT (P1-2): `previously_healthy`
   is HISTORICAL and monotonic-within-a-boot — once a device has positively been
   established as healthy it stays latched even if it later disappears. So this
   never unconditionally overwrites the latch. Passing ever_present=true latches
   health; passing ever_present=false records "not seen this discovery pass" but
   does NOT forget that the device was previously known healthy. To CLEAR health
   only Init() may be used (fresh boot). A device that has never responded stays
   never_present=true and previously_healthy=false. Slot must be < 8. */
static inline void I2cBusHealth_SetDeviceKnown(I2cBusHealth *h, uint8_t slot, bool ever_present)
{
    if (h == NULL || slot >= 8U) return;
    if (ever_present)
    {
        h->never_present[slot] = false;
        h->previously_healthy[slot] = true;   /* monotonic latch */
    }
    else
    {
        /* Device currently not seen: keep never_present=true only if it was
           never established as healthy; but never un-latch a previously healthy
           device here. */
        if (!h->previously_healthy[slot])
            h->never_present[slot] = true;
    }
}

/* Positive healthy evidence: pass true when an actual successful probe / init /
   transaction / READY / valid-sample transition was observed this pass. Latches
   previously_healthy (monotonic within boot) and clears never_present when true.
   Passing false is a no-op (a momentary absence must never un-latch history).
   This is the preferred API for App to confirm real device health (P1-2),
   distinct from mere current-absence status. Slot must be < 8. */
static inline void I2cBusHealth_MarkHealth(I2cBusHealth *h, uint8_t slot, bool observed)
{
    if (h == NULL || slot >= 8U) return;
    if (observed)
    {
        h->never_present[slot] = false;
        h->previously_healthy[slot] = true;
    }
}

static inline bool I2cBusHealth_ShouldRecover(const I2cBusHealth *h)
{
    return h != NULL && h->state == I2C_BUS_HEALTH_RECOVERING;
}

/* True only when a recovery attempt is permitted by both evidence and cooldown. */
static inline bool I2cBusHealth_RecoveryEligible(const I2cBusHealth *h, uint32_t now)
{
    if (h == NULL || h->state != I2C_BUS_HEALTH_RECOVERING)
        return false;
    if (h->transport_evidence < RECOVERY_BUS_EVIDENCE_MIN)
        return false;
    /* Cooldown: enough time must have passed since the last attempt. Wrap-safe.
       Before any attempt, eligibility is allowed immediately. */
    if (!h->ever_recovered)
        return true;
    return RecoveryPolicy_Elapsed(now, h->last_recovery_ms, RECOVERY_BUS_COOLDOWN_MS);
}

/* Expire an aged window and recount evidence from the current tick. Returns the
   (up-to-8-capped) distinct previously-healthy slots whose transport failure is
   still inside the window. */
static inline uint32_t I2cBusHealth_CountWithinWindow(I2cBusHealth *h, uint32_t now)
{
    uint32_t count = 0U;
    {
        int i;
        for (i = 0; i < 8; i++)
        {
            uint32_t t = h->last_transport_ms[i];
            if (t == 0U)
                continue;
            if (RecoveryPolicy_WindowWithin(now, t, RECOVERY_BUS_WINDOW_MS))
                count++;
            else
                h->last_transport_ms[i] = 0U;   /* stale: drop for good */
        }
    }
    h->transport_evidence = count;
    return count;
}

/* Report one completed operation result for a device slot. Returns true when a
   shared-bus recovery is warranted AND eligible (evidence + cooldown). A device
   that was never present reporting NOT_FOUND contributes no evidence. */
static inline bool I2cBusHealth_Report(I2cBusHealth *h, uint8_t slot, DriverStatus status, uint32_t now)
{
    if (h == NULL || slot >= 8U) return false;

    RecoveryFailClass cls = RecoveryPolicy_Classify(status);

    /* Absent from a never-present device: NOT bus evidence. A previously-present
       device disappearing is device-local; also not counted as bus evidence. */
    if (cls == FAIL_CLASS_ABSENT)
        return false;

    /* Device-local fault (DEVICE_ERROR) or data-integrity (CRC/VERIFY): never
       shared-bus evidence. */
    if (cls == FAIL_CLASS_DEVICE_LOCAL || cls == FAIL_CLASS_DATA)
        return false;

    if (cls == FAIL_CLASS_TRANSPORT)
    {
        if (!h->previously_healthy[slot])
            return false;
        h->last_transport_ms[slot] = now;
        if (!h->window_open)
        {
            h->window_open = true;
            h->window_open_ms = now;
        }
        /* Age the epoch: if the window has run past RECOVERY_BUS_WINDOW_MS since
           it opened, re-anchor now and recount so stale evidence expires. */
        if (RecoveryPolicy_Elapsed(now, h->window_open_ms, RECOVERY_BUS_WINDOW_MS))
        {
            h->window_open_ms = now;
            I2cBusHealth_CountWithinWindow(h, now);
        }
        h->transport_evidence = I2cBusHealth_CountWithinWindow(h, now);

        if (h->transport_evidence >= RECOVERY_BUS_EVIDENCE_MIN)
        {
            h->state = I2C_BUS_HEALTH_RECOVERING;
        }
    }
    return I2cBusHealth_RecoveryEligible(h, now);
}

/* Begin a recovery attempt. MUST be called immediately before I2cBus_Recover.
   Increments attempts, records the cooldown base, and CLEARS the whole evidence
   epoch so no old evidence can participate in a later decision — whether this
   attempt succeeds or fails. */
static inline void I2cBusHealth_BeginRecovery(I2cBusHealth *h, uint32_t now)
{
    if (h == NULL) return;
    h->bus_recovery_attempts++;
    h->ever_recovered = true;
    h->last_recovery_ms = now;
    h->state = I2C_BUS_HEALTH_OK;
    h->window_open = false;
    h->window_open_ms = 0U;
    h->transport_evidence = 0U;
    {
        int i;
        for (i = 0; i < 8; i++)
            h->last_transport_ms[i] = 0U;
    }
}

/* Complete a successful recovery attempt. */
static inline void I2cBusHealth_OnRecoverySuccess(I2cBusHealth *h)
{
    if (h == NULL) return;
    h->bus_recovery_successes++;
}

/* Complete a failed recovery attempt. */
static inline void I2cBusHealth_OnRecoveryFailure(I2cBusHealth *h)
{
    if (h == NULL) return;
    h->bus_recovery_failures++;
}

/* Legacy: bus_recovery_count == successes (kept for existing diagnostics). */
static inline uint32_t I2cBusHealth_GetBusRecoveryCount(const I2cBusHealth *h)
{
    return h != NULL ? h->bus_recovery_successes : 0U;
}

static inline uint32_t I2cBusHealth_GetBusRecoveryAttempts(const I2cBusHealth *h)
{
    return h != NULL ? h->bus_recovery_attempts : 0U;
}

static inline uint32_t I2cBusHealth_GetBusRecoverySuccesses(const I2cBusHealth *h)
{
    return h != NULL ? h->bus_recovery_successes : 0U;
}

static inline uint32_t I2cBusHealth_GetBusRecoveryFailures(const I2cBusHealth *h)
{
    return h != NULL ? h->bus_recovery_failures : 0U;
}

#endif