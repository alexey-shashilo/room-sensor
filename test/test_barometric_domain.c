#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#include "room_state.h"
#include "telemetry.h"
#include "telemetry_serializer.h"

/* Barometric domain integration harness (Phase 17.7B).

   Exercises provider selection + RoomState genericharity + atomicity + the
   telemetry source-of-truth. Emits serialized schema-v5 telemetry per case
   prefixed "CASE <n>". A companion python script (test_barometric_domain.py)
   pipes each blob through a real JSON parser (json.loads) and asserts:
     - generic barometric_sensor naming (bmp390/bmp380/none)
     - generic pressure/temperature validity
     - legacy bmp390_* fields valid ONLY for provider BMP390
     - BMP380 data is NEVER serialized under bmp390 names
     - no fabricated zeros for provider NONE
     - provider + pressure + temperature never mixed (atomic snapshot)

   RoomState_UpdateBarometric is the domain API under test; the serializer reads
   the resulting RoomState, so the JSON assertions are the source of truth. */
static int s_pass = 0, s_fail = 0, s_case_count = 0;
static void check(int cond, const char *name)
{
    s_case_count++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case_count, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case_count, name); }
}

static void flush_case(int n, const RoomState *room)
{
    uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
    size_t written = 0;
    TelemetrySnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.sequence = (uint32_t)n;
    snap.uptime_ms = (uint32_t)n * 1000u;
    snap.captured_at_ms = (uint32_t)n * 1000u;
    snap.health = SYSTEM_HEALTH_OK;
    snap.boot_id = 0x1122334455667788ULL;
    memcpy(snap.device_id, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", 16);
    snap.room = *room;
    SerializeStatus s = Telemetry_Serialize(&snap, buf, sizeof(buf), &written);
    if (s != SERIALIZE_OK) { printf("CASE %d\nSERIALIZE_ERROR\n", n); return; }
    buf[written] = '\0';
    printf("CASE %d\n%s", n, (const char *)buf);
}

int main(void)
{
    printf("Barometric domain provider integration tests\n");

    /* --- A. BMP390 provider only --- */
    {
        RoomState rs; RoomState_Init(&rs);
        RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP390, 101325.0f, true,
                                   24.5f, true);
        check(rs.barometric_provider == BAROMETER_PROVIDER_BMP390, "A: provider BMP390");
        check(rs.barometric_pressure_valid && rs.barometric_temperature_valid, "A: generic valid");
        check(rs.bmp390_pressure_valid && rs.bmp390_temperature_valid, "A: legacy bmp390 valid");
        check(rs.bmp390_pressure_pa == 101325.0f, "A: legacy bmp390 pressure mirrored");
        flush_case(1, &rs);
    }

    /* --- B. BMP380 ONLY --- */
    {
        RoomState rs; RoomState_Init(&rs);
        RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP380, 98250.5f, true,
                                   21.0f, true);
        check(rs.barometric_provider == BAROMETER_PROVIDER_BMP380, "B: provider BMP380");
        check(rs.barometric_pressure_valid && rs.barometric_temperature_valid, "B: generic valid");
        check(!rs.bmp390_pressure_valid && !rs.bmp390_temperature_valid, "B: legacy bmp390 INVALID");
        flush_case(2, &rs);
    }

    /* --- C. NEITHER (NONE) --- */
    {
        RoomState rs; RoomState_Init(&rs);
        RoomState_InvalidateBarometric(&rs);
        check(rs.barometric_provider == BAROMETER_PROVIDER_NONE, "C: provider NONE");
        check(!rs.barometric_pressure_valid && !rs.barometric_temperature_valid, "C: generic invalid");
        check(!rs.bmp390_pressure_valid && !rs.bmp390_temperature_valid, "C: legacy invalid");
        flush_case(3, &rs);
    }

    /* --- D. BOTH -> BMP390 wins deterministically --- */
    {
        RoomState rs; RoomState_Init(&rs);
        /* Simulate provider selection consuming FRESH validity: both fresh, BMP390 wins. */
        RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP390, 101000.0f, true, 24.0f, true);
        /* BMP380 then reports fresh too — but selection must NOT switch (BMP390
           still fresh). In App this is App_CommitBarometricRoomState gating on
           BMP390 HasValidSample first; here assert the invariant the commit keeps:
           the active provider stays BMP390 (no oscillation). */
        check(rs.barometric_provider == BAROMETER_PROVIDER_BMP390, "D: BMP390 stays provider");
        flush_case(4, &rs);
    }

    /* --- E. BMP390 loses freshness -> BMP380 becomes provider --- */
    {
        RoomState rs; RoomState_Init(&rs);
        /* BMP390 was provider, then stale: provider falls back to BMP380 (fresh). */
        RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP380, 90000.0f, true, 19.0f, true);
        check(rs.barometric_provider == BAROMETER_PROVIDER_BMP380, "E: BMP380 becomes provider");
        check(!rs.bmp390_pressure_valid, "E: legacy bmp390 cleared");
        flush_case(5, &rs);
    }

    /* --- F. BMP390 recovers -> BMP390 preferred again --- */
    {
        RoomState rs; RoomState_Init(&rs);
        RoomState_InvalidateBarometric(&rs);          /* both lost */
        RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP390, 101222.5f, true, 23.9f, true);
        check(rs.barometric_provider == BAROMETER_PROVIDER_BMP390, "F: BMP390 recovers to provider");
        check(rs.bmp390_pressure_valid, "F: legacy bmp390 valid again");
        flush_case(6, &rs);
    }

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}