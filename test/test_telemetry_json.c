#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#include "telemetry.h"
#include "telemetry_serializer.h"
#include "room_state.h"

/* JSON wire-format regression harness.

   Emits one serialized schema-v3 telemetry payload per case on stdout, each
   prefixed with "CASE <n>". A companion python script (test_telemetry_json.py)
   pipes each blob through a real JSON parser (json.loads) and asserts the
   decoded values. This proves every Telemetry_Serialize() output is valid
   JSON — the substring-only tests could not catch malformed output. */

static void flush_case(int n, const TelemetrySnapshot *snap)
{
    uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
    size_t written = 0;
    SerializeStatus s = Telemetry_Serialize(snap, buf, sizeof(buf), &written);
    if (s != SERIALIZE_OK)
    {
        printf("CASE %d\nSERIALIZE_ERROR\n", n);
        return;
    }
    buf[written] = '\0';
    printf("CASE %d\n%s", n, (const char *)buf);
}

int main(void)
{
    /* 1: all SCD41 values valid + illuminance. */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 42;
        snap.uptime_ms = 10000;
        snap.captured_at_ms = 10000;
        snap.health = SYSTEM_HEALTH_OK;
        memcpy(snap.device_id,
               "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", 16);
        snap.room.illuminance_lux = 72.4f;
        snap.room.illuminance_valid = true;
        snap.room.co2_ppm = 1006.0f;
        snap.room.co2_valid = true;
        snap.room.scd41_temperature_c = 27.1f;
        snap.room.scd41_temperature_valid = true;
        snap.room.scd41_humidity_pct = 49.0f;
        snap.room.scd41_humidity_valid = true;
        flush_case(1, &snap);
    }

    /* 2: all SCD41 values invalid. */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 43;
        snap.uptime_ms = 11000;
        snap.captured_at_ms = 11000;
        snap.health = SYSTEM_HEALTH_DEGRADED;
        snap.room.co2_valid = false;
        snap.room.scd41_temperature_valid = false;
        snap.room.scd41_humidity_valid = false;
        flush_case(2, &snap);
    }

    /* 3: mixed validity (co2 valid, T invalid, RH valid). */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 44;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.co2_ppm = 500.0f;
        snap.room.co2_valid = true;
        snap.room.scd41_temperature_valid = false;
        snap.room.scd41_humidity_pct = 30.0f;
        snap.room.scd41_humidity_valid = true;
        flush_case(3, &snap);
    }

    /* 4: maximum legal CO2 = 40000. */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 45;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.co2_ppm = 40000.0f;
        snap.room.co2_valid = true;
        flush_case(4, &snap);
    }

    /* 5: CO2 over max (> 40000) -> must be serialized invalid. */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 46;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.co2_ppm = 40001.0f;
        snap.room.co2_valid = true;
        flush_case(5, &snap);
    }

    /* 6: negative CO2 (valid flag hacked true) -> invalid. */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 47;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.co2_ppm = -5.0f;
        snap.room.co2_valid = true;
        flush_case(6, &snap);
    }

    /* 7: CO2 = NaN -> invalid (never a numeric value). */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 48;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.co2_ppm = NAN;
        snap.room.co2_valid = true;
        flush_case(7, &snap);
    }

    /* 8: CO2 = +Inf -> invalid. */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 49;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.co2_ppm = INFINITY;
        snap.room.co2_valid = true;
        flush_case(8, &snap);
    }

    /* 9: CO2 = -Inf -> invalid. */
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 50;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.co2_ppm = -INFINITY;
        snap.room.co2_valid = true;
        flush_case(9, &snap);
    }

    return 0;
}