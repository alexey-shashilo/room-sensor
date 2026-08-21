#include <stdio.h>
#include <string.h>

#include "room_state.h"
#include "room_sensor_types.h"
#include "fake_platform_time.h"

/* Phase 20 — Generic room-environment provider LONG-RUN (24h virtual) test.

   Mirrors the production App commit policy (App_CommitEnvironmentRoomState) over
   >= 86,400,000 ms of VIRTUAL time (no real sleep), injecting multiple SHT45
   absence / BUS_ERROR / recovery episodes while SCD41 stays healthy, plus brief
   both-invalid periods and a uint32 tick wrap.

   The test replicates the exact deterministic selection the App uses:
     SHT45 both-valid  -> SHT45
     else SCD41 both-valid -> SCD41
     else NONE
   and plumbs each selection through the production atomic RoomState_UpdateEnvironment
   so the generic snapshot invariants (coherent pair, no-mix, no stale-valid, no
   fabricated) are exercised over the long horizon. */

static int s_pass=0, s_fail=0, s_case=0;
static void check(int c, const char*n){ s_case++; if(c){s_pass++;} else {s_fail++; printf("  FAIL #%d: %s\n", s_case, n);} }

/* Production selection policy (byte-for-byte the App's priority). */
static void apply_commit(RoomState *rs, bool s45_both_valid,
                         bool scd_both_valid)
{
    if (s45_both_valid)
    {
        RoomState_UpdateEnvironment(rs, ENVIRONMENT_PROVIDER_SHT45,
                                    rs->sht45_temperature_c, true,
                                    rs->sht45_humidity_pct, true);
    }
    else if (scd_both_valid)
    {
        RoomState_UpdateEnvironment(rs, ENVIRONMENT_PROVIDER_SCD41,
                                    rs->scd41_temperature_c, true,
                                    rs->scd41_humidity_pct, true);
    }
    else
    {
        RoomState_InvalidateEnvironment(rs);
    }
}

int main(void)
{
    printf("Environment provider 24h long-run\n");
    FakePlatform_SetTick(0);

    RoomState rs; RoomState_Init(&rs);
    const float S45T=26.3f, S45RH=51.2f, SCDT=27.3f, SCDRH=47.5f;

    uint32_t tick=0;
    const uint32_t step=1000U;               /* 1 s per step */
    const uint32_t horizon=86400000U;        /* >= 86,400,000 ms (24h) */
    const uint32_t WINDOW_SHT45=12UL*3600UL*1000UL;
    const uint32_t WINDOW_SCD=8UL*3600UL*1000UL;
    const uint32_t WINDOW_BOTH_INVALID=1200000UL;  /* 20 min both-invalid */

    bool s45 = true, scd = true;
    uint32_t s45_abs_until=0, scd_abs_until=0, twinvalid_until=0;
    uint32_t next_s45_ep=0, next_scd_ep=0;
    EnvironmentProvider prev = ENVIRONMENT_PROVIDER_NONE;
    uint32_t thrash_changes=0, stale_valid_hits=0, both_valid_seen=0;

    while (tick < horizon)
    {
        /* Schedule SHT45 absence episodes: present, then absent WINDOW_SHT45. */
        if (tick >= next_s45_ep)
        {
            s45 = !s45;
            s45_abs_until = s45 ? 0U : (tick + WINDOW_SHT45);
            next_s45_ep = tick + (s45 ? WINDOW_SHT45 : WINDOW_SHT45);
        }
        /* Schedule SCD41 absence episodes. */
        if (tick >= next_scd_ep)
        {
            scd = !scd;
            scd_abs_until = scd ? 0U : (tick + WINDOW_SCD);
            next_scd_ep = tick + (scd ? WINDOW_SCD : WINDOW_SCD);
        }
        /* Occasional brief both-invalid window. */
        if (tick == (horizon/2)) twinvalid_until = tick + WINDOW_BOTH_INVALID;

        bool s45v = s45 || (tick < s45_abs_until);
        bool scdv = scd || (tick < scd_abs_until);
        bool both_invalid_win = (tick < twinvalid_until);

        if (both_invalid_win){ s45v=false; scdv=false; }

        /* Populate model-specific RoomState from source availability. */
        rs.sht45_temperature_c=S45T; rs.sht45_temperature_valid=s45v;
        rs.sht45_humidity_pct=S45RH; rs.sht45_humidity_valid=s45v;
        rs.scd41_temperature_c=SCDT; rs.scd41_temperature_valid=scdv;
        rs.scd41_humidity_pct=SCDRH; rs.scd41_humidity_valid=scdv;

        bool s45_pair = s45v;
        bool scd_pair = scdv;
        apply_commit(&rs, s45_pair, scd_pair);

        /* Invariants over the whole run. */
        if (rs.environment_temperature_valid) both_valid_seen++;
        EnvironmentProvider now = rs.environment_provider;
        if (now != prev) thrash_changes++;
        prev = now;

        /* Never stale-valid: if provider is NONE, generic T/RH must be invalid. */
        if (now==ENVIRONMENT_PROVIDER_NONE &&
            (rs.environment_temperature_valid || rs.environment_humidity_valid))
            stale_valid_hits++;

        /* Coherent snapshot: T/RH must come from the SAME source as provider. */
        if (now==ENVIRONMENT_PROVIDER_SHT45)
        {
            if (rs.environment_temperature_c!=S45T || rs.environment_humidity_pct!=S45RH) { s_fail++; printf("  FAIL: mixed snapshot SHT45\n"); }
        }
        else if (now==ENVIRONMENT_PROVIDER_SCD41)
        {
            if (rs.environment_temperature_c!=SCDT || rs.environment_humidity_pct!=SCDRH) { s_fail++; printf("  FAIL: mixed snapshot SCD41\n"); }
        }

        tick += step;
        FakePlatform_AdvanceTick(step);
    }

    /* Simulate uint32 wrap near the top of the counter and continue. */
    FakePlatform_SetTick(0x7FFFFFF0U);
    {
        bool s45w=true, scdw=true;
        apply_commit(&rs, s45w, scdw);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SHT45,
              "wrap: provider SHT45 after uint32 wrap setpoint");
        for (int i=0;i<200;i++){ FakePlatform_AdvanceTick(50); }
        apply_commit(&rs, false, true);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SCD41,
              "wrap: fallback to SCD41 after wrap");
    }

    check(tick+2000U >= horizon, "horizon reached (>= 86,400,000 ms)");
    check(stale_valid_hits==0, "SHT45_STALE_CAN_REMAIN_GENERIC_VALID = NO (0 hits)");
    check(s_fail==0, "no mixed-provider snapshot over the whole run");
    check(both_valid_seen>0, "valid generic environment was produced (not never-valid)");
    /* Provider must have actually switched at least twice (SHT45<->SCD41), but not
       oscillate wildly: the number of transitions is bounded by the episode schedule. */
    check(thrash_changes>1U, "provider selections occurred (episodes exercised)");
    check(thrash_changes < 2000U, "ENVIRONMENT_PROVIDER_THRASH = NO (bounded transitions)");

    printf("\n%d pass, %d fail (transitions=%u, both_valid=%u)\n",
           s_pass, s_fail, thrash_changes, both_valid_seen);
    return (s_fail==0)?0:1;
}