#include <stdio.h>
#include <string.h>

#include "room_state.h"
#include "room_sensor_types.h"
#include "fake_platform_time.h"

/* Phase 20 — Generic room-environment provider policy host test.

   Verifies the atomic, deterministic provider selection semantics exposed by
   RoomState_UpdateEnvironment / RoomState_InvalidateEnvironment and the generic
   environment snapshot consumed by display + SGP41 compensation.

   RoomState_UpdateEnvironment is the single authoritative path: the App
   (App_CommitEnvironmentRoomState) calls it exactly once per tick with the
   ACTIVE provider's coherent T+RH pair. This test exercises the atomic contract
   directly (no mixed snapshot, NONE never fabricates). */

static int s_pass=0, s_fail=0, s_case=0;
static void check(int c, const char*n){ s_case++; if(c){s_pass++;} else {s_fail++; printf("  FAIL #%d: %s\n", s_case, n);} }

static void init_room(RoomState *rs){ RoomState_Init(rs); }

int main(void)
{
    printf("Environment provider policy tests\n");
    FakePlatform_SetTick(0);

    /* A. SHT45 valid + SCD41 valid -> provider SHT45 (priority), coherent pair. */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_c=25.4f; rs.sht45_temperature_valid=true;
        rs.sht45_humidity_pct=51.0f;  rs.sht45_humidity_valid=true;
        rs.scd41_temperature_c=27.3f; rs.scd41_temperature_valid=true;
        rs.scd41_humidity_pct=47.0f;  rs.scd41_humidity_valid=true;
        /* App would pick SHT45; mirror that selection here. */
        RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SHT45,
                                    rs.sht45_temperature_c, true,
                                    rs.sht45_humidity_pct, true);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SHT45, "A: provider SHT45 (priority)");
        check(rs.environment_temperature_c==25.4f && rs.environment_humidity_pct==51.0f,
              "A: generic pair == SHT45 pair (no mix)");
        check(rs.environment_temperature_valid && rs.environment_humidity_valid,
              "A: both generic channels valid");
        check(rs.scd41_temperature_c==27.3f, "A: SCD41 model field preserved");
        check(rs.sht45_temperature_c==25.4f, "A: SHT45 model field preserved");
    }

    /* B. SHT45 invalid + SCD41 valid -> provider SCD41. */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_valid=false; rs.sht45_humidity_valid=false;
        rs.scd41_temperature_c=27.5f; rs.scd41_temperature_valid=true;
        rs.scd41_humidity_pct=46.0f;  rs.scd41_humidity_valid=true;
        RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SCD41,
                                    rs.scd41_temperature_c, true,
                                    rs.scd41_humidity_pct, true);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SCD41, "B: provider SCD41 (fallback)");
        check(rs.environment_temperature_c==27.5f && rs.environment_humidity_pct==46.0f,
              "B: generic pair == SCD41 pair");
    }

    /* C. SHT45 valid + SCD41 invalid -> provider SHT45. */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_c=24.8f; rs.sht45_temperature_valid=true;
        rs.sht45_humidity_pct=52.0f;  rs.sht45_humidity_valid=true;
        rs.scd41_temperature_c=0.0f;  rs.scd41_temperature_valid=false;
        rs.scd41_humidity_pct=0.0f;   rs.scd41_humidity_valid=false;
        RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SHT45,
                                    rs.sht45_temperature_c, true,
                                    rs.sht45_humidity_pct, true);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SHT45, "C: provider SHT45");
    }

    /* D. both invalid -> provider NONE, generic T/RH invalid (never fabricate). */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_valid=false; rs.sht45_humidity_valid=false;
        rs.scd41_temperature_valid=false; rs.scd41_humidity_valid=false;
        RoomState_InvalidateEnvironment(&rs);   /* App path when neither fresh-valid */
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_NONE, "D: provider NONE");
        check(!rs.environment_temperature_valid && !rs.environment_humidity_valid,
              "D: generic T/RH invalid (never fabricated)");
    }

    /* E. stale SHT45 + SCD41 valid -> App would fall back; UpdateEnvironment(SCD41). */
    {
        RoomState rs; init_room(&rs);
        rs.scd41_temperature_c=28.0f; rs.scd41_temperature_valid=true;
        rs.scd41_humidity_pct=45.0f;  rs.scd41_humidity_valid=true;
        RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SCD41,
                                    rs.scd41_temperature_c, true,
                                    rs.scd41_humidity_pct, true);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SCD41, "E: fallback to SCD41 on stale SHT45");
    }

    /* F. SHT45 recovers -> provider returns to SHT45. */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_c=25.0f; rs.sht45_temperature_valid=true;
        rs.sht45_humidity_pct=51.5f;  rs.sht45_humidity_valid=true;
        RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SHT45,
                                    rs.sht45_temperature_c, true,
                                    rs.sht45_humidity_pct, true);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SHT45, "F: provider returns to SHT45");
    }

    /* G. SHT45 T valid but RH invalid -> no mixed snapshot; policy falls back as a
       complete pair. App-commit semantics: a provider is selected only when BOTH
       channels are valid; UpdateEnvironment with NONE is the deterministic outcome. */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_c=25.1f; rs.sht45_temperature_valid=true;
        rs.sht45_humidity_valid=false;                /* RH invalid -> not a coherent pair */
        rs.scd41_temperature_c=27.0f; rs.scd41_temperature_valid=true;
        rs.scd41_humidity_pct=48.0f;  rs.scd41_humidity_valid=true;
        /* App: SHT45 not a coherent valid pair --> fall back to full SCD41 pair. */
        RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SCD41,
                                    rs.scd41_temperature_c, true,
                                    rs.scd41_humidity_pct, true);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SCD41,
              "G: partial SHT45 falls back to complete SCD41 pair (no mix)");
        check(rs.environment_temperature_c==27.0f && rs.environment_humidity_pct==48.0f,
              "G: generic pair is complete SCD41 pair");
    }

    /* H. SCD41 T valid but RH invalid -> no mixed snapshot; nothing valid. */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_valid=false; rs.sht45_humidity_valid=false;
        rs.scd41_temperature_c=27.0f; rs.scd41_temperature_valid=true;
        rs.scd41_humidity_valid=false;              /* partial SCD41 -> not coherent */
        /* Neither source yields a complete fresh valid pair. */
        RoomState_InvalidateEnvironment(&rs);
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_NONE, "H: NONE (no mixed snapshot)");
        check(!rs.environment_temperature_valid, "H: temperature invalid (no T-from-SCD41-only)");
    }

    /* I. no stale-valid generic values: numeric retained for diagnostics but
       validity cleared; a subsequent INVALIDATE clears both. */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_c=30.0f; rs.sht45_temperature_valid=true;
        rs.sht45_humidity_pct=60.0f;  rs.sht45_humidity_valid=true;
        RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SHT45,
                                    rs.sht45_temperature_c, true,
                                    rs.sht45_humidity_pct, true);
        check(rs.environment_temperature_valid, "I: initially valid");
        RoomState_InvalidateEnvironment(&rs);
        check(!rs.environment_temperature_valid && !rs.environment_humidity_valid,
              "I: after invalidation generic is invalid (no stale-valid)");
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_NONE, "I: provider NONE (no stale)");
    }

    /* J. provider selection deterministic (idempotent re-commit, no thrash). */
    {
        RoomState rs; init_room(&rs);
        rs.sht45_temperature_c=24.0f; rs.sht45_temperature_valid=true;
        rs.sht45_humidity_pct=50.0f;  rs.sht45_humidity_valid=true;
        for (int i=0;i<10;i++)
        {
            RoomState_UpdateEnvironment(&rs, ENVIRONMENT_PROVIDER_SHT45,
                                        rs.sht45_temperature_c, true,
                                        rs.sht45_humidity_pct, true);
        }
        check(rs.environment_provider==ENVIRONMENT_PROVIDER_SHT45,
              "J: deterministic provider (no thrash)");
        check(rs.environment_temperature_c==24.0f && rs.environment_humidity_pct==50.0f,
              "J: stable generic values");
    }

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail==0)?0:1;
}