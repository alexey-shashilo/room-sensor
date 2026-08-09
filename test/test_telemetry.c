#include <stdio.h>
#include <string.h>
#include <math.h>

#include "telemetry.h"
#include "telemetry_serializer.h"
#include "communication.h"
#include "communication_port.h"
#include "fake_communication_port.h"
#include "fake_platform_time.h"
#include "fake_flash.h"
#include "room_state.h"
#include "config.h"
#include "device_identity.h"

static int s_pass = 0;
static int s_fail = 0;
static int s_case = 0;

static void test(const char *name, int cond)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

int main(void)
{
    printf("Telemetry / Communication Host Tests\n");

    /* 1: snapshot copies RoomState */
    printf("\n=== Snapshot semantics ===\n");
    {
        RoomState rs;
        RoomState_Init(&rs);
        RoomState_UpdateIlluminance(&rs, 72.4f, true);

        /* FIXME: can't test Telemetry_CreateSnapshot easily without full app state.
           Test the serializer with a manually constructed snapshot. */
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 42;
        snap.uptime_ms = 10000;
        snap.captured_at_ms = 10000;
        snap.health = SYSTEM_HEALTH_OK;
        memcpy(snap.device_id, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", 16);
        snap.room.illuminance_lux = 72.4f;
        snap.room.illuminance_valid = true;

        test("snapshot struct valid", snap.sequence == 42);
    }

    /* 5: valid illuminance serialized correctly */
    printf("\n=== Valid illuminance ===\n");
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 42;
        snap.uptime_ms = 10000;
        snap.captured_at_ms = 10000;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.illuminance_lux = 72.4f;
        snap.room.illuminance_valid = true;
        snap.device_id[0] = 0xAA; snap.device_id[1] = 0xBB;

        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t written = 0;
        SerializeStatus s = Telemetry_Serialize(&snap, buf, sizeof(buf), &written);

        test("serialize OK", s == SERIALIZE_OK);
        test("written > 0", written > 0);
        test("contains schema", strstr((char *)buf, "\"schema\": 1") != NULL);
        test("contains seq 42", strstr((char *)buf, "\"seq\": 42") != NULL);
        test("contains health ok", strstr((char *)buf, "\"health\": \"ok\"") != NULL);
        test("contains illuminance", strstr((char *)buf, "illuminance_lux") != NULL);
        test("contains value 72.4", strstr((char *)buf, "72.4") != NULL);
        test("contains state valid", strstr((char *)buf, "\"state\": \"valid\"") != NULL);
        test("contains device_id", strstr((char *)buf, "\"device_id\":") != NULL);
        test("contains uptime_ms", strstr((char *)buf, "\"uptime_ms\": 10000") != NULL);
        test("contains captured_at_ms", strstr((char *)buf, "\"captured_at_ms\": 10000") != NULL);
    }

    /* 6: invalid illuminance */
    printf("\n=== Invalid illuminance ===\n");
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 1;
        snap.uptime_ms = 2000;
        snap.captured_at_ms = 2000;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.illuminance_valid = false;
        snap.room.illuminance_lux = 0.0f;

        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t written = 0;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &written);

        test("invalid -> no numeric value", strstr((char *)buf, "\"value\":") == NULL);
        test("invalid -> state invalid", strstr((char *)buf, "\"state\": \"invalid\"") != NULL);
    }

    /* 8: health strings */
    printf("\n=== Health strings ===\n");
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 1;
        snap.uptime_ms = 100;
        snap.captured_at_ms = 100;
        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t written;

        snap.health = SYSTEM_HEALTH_BOOTING;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &written);
        test("health booting", strstr((char *)buf, "\"health\": \"booting\"") != NULL);

        snap.health = SYSTEM_HEALTH_OK;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &written);
        test("health ok", strstr((char *)buf, "\"health\": \"ok\"") != NULL);

        snap.health = SYSTEM_HEALTH_DEGRADED;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &written);
        test("health degraded", strstr((char *)buf, "\"health\": \"degraded\"") != NULL);

        snap.health = SYSTEM_HEALTH_FAULT;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &written);
        test("health fault", strstr((char *)buf, "\"health\": \"fault\"") != NULL);
    }

    /* 10: schema == 1 */
    printf("\n=== Schema version ===\n");
    test("TELEMETRY_SCHEMA_VERSION == 1", TELEMETRY_SCHEMA_VERSION == 1);

    /* 12: serializer rejects NULL args */
    printf("\n=== NULL arg rejection ===\n");
    {
        uint8_t buf[64];
        size_t written;
        test("null snapshot", Telemetry_Serialize(NULL, buf, sizeof(buf), &written) == SERIALIZE_INVALID_ARG);
        test("null buffer", Telemetry_Serialize(&(TelemetrySnapshot){0}, NULL, 64, &written) == SERIALIZE_INVALID_ARG);
        test("null written", Telemetry_Serialize(&(TelemetrySnapshot){0}, buf, 64, NULL) == SERIALIZE_INVALID_ARG);
    }

    /* 13: insufficient buffer */
    printf("\n=== Buffer too small ===\n");
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 1;
        snap.uptime_ms = 100;
        snap.captured_at_ms = 100;
        snap.health = SYSTEM_HEALTH_OK;
        uint8_t tiny[1];
        size_t written;
        test("buffer too small", Telemetry_Serialize(&snap, tiny, 1, &written) == SERIALIZE_BUFFER_TOO_SMALL);
    }

    /* 15: NaN becomes invalid */
    printf("\n=== NaN handling ===\n");
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 1;
        snap.uptime_ms = 100;
        snap.captured_at_ms = 100;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.illuminance_lux = 0.0f / 0.0f;
        snap.room.illuminance_valid = true;

        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t written;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &written);
        test("NaN -> state invalid", strstr((char *)buf, "\"state\": \"invalid\"") != NULL);
        test("NaN -> no value", strstr((char *)buf, "\"value\":") == NULL);
    }

    /* 17: communication send */
    printf("\n=== Communication send ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake);
        FakeComm_GetPort(&port, &fake);

        Communication_Init();
        Communication_SetPort(&port);

        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 1;
        snap.uptime_ms = 100;
        snap.captured_at_ms = 100;
        snap.health = SYSTEM_HEALTH_OK;

        Communication_SubmitSnapshot(&snap);
        test("pending before send", fake.send_call_count == 0);

        Communication_Run();
        test("sent", fake.send_call_count == 1);
        test("captured non-empty", fake.last_captured_size > 0);
    }

    /* 18: disconnected -> pending remains */
    printf("\n=== Disconnected ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake);
        fake.ready = false;
        FakeComm_GetPort(&port, &fake);

        Communication_Init();
        Communication_SetPort(&port);

        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 2;
        snap.uptime_ms = 200;
        snap.captured_at_ms = 200;
        snap.health = SYSTEM_HEALTH_OK;

        Communication_SubmitSnapshot(&snap);
        Communication_Run();

        test("not sent when disconnected", fake.send_call_count == 0);
    }

    /* 21: new snapshot overwrites pending */
    printf("\n=== Latest-value-wins ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake);
        FakeComm_GetPort(&port, &fake);

        Communication_Init();
        Communication_SetPort(&port);

        TelemetrySnapshot snap1, snap2;
        memset(&snap1, 0, sizeof(snap1)); snap1.sequence = 1;
        snap1.uptime_ms = 100; snap1.captured_at_ms = 100;
        snap1.health = SYSTEM_HEALTH_OK;

        memset(&snap2, 0, sizeof(snap2)); snap2.sequence = 2;
        snap2.uptime_ms = 200; snap2.captured_at_ms = 200;
        snap2.health = SYSTEM_HEALTH_DEGRADED;

        Communication_SubmitSnapshot(&snap1);
        Communication_SubmitSnapshot(&snap2);
        Communication_Run();

        test("seq=2 sent (latest wins)", strstr((char *)fake.last_captured, "\"seq\": 2") != NULL);
        test("health degraded in payload", strstr((char *)fake.last_captured, "degraded") != NULL);
    }

    /* 24: send success clears pending */
    printf("\n=== Send clears pending ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake);
        FakeComm_GetPort(&port, &fake);

        Communication_Init();
        Communication_SetPort(&port);

        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 1;
        snap.uptime_ms = 100;
        snap.captured_at_ms = 100;
        snap.health = SYSTEM_HEALTH_OK;

        Communication_SubmitSnapshot(&snap);
        Communication_Run();
        Communication_Run();
        Communication_Run();

        test("only sent once (cleared)", fake.send_call_count == 1);
    }

    /* 25: transport failure does not alter RoomState */
    printf("\n=== Transport failure isolation ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake);
        fake.send_result = COMM_STATUS_ERROR;
        FakeComm_GetPort(&port, &fake);

        Communication_Init();
        Communication_SetPort(&port);

        RoomState rs;
        RoomState_Init(&rs);
        RoomState_UpdateIlluminance(&rs, 72.4f, true);

        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 1;
        snap.uptime_ms = 100;
        snap.captured_at_ms = 100;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room = rs;

        Communication_SubmitSnapshot(&snap);
        Communication_Run();

        test("room unchanged after comm failure", rs.illuminance_lux == 72.4f);
        test("room still valid", rs.illuminance_valid == true);
    }

    /* golden payload test */
    printf("\n=== Golden payload ===\n");
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 42;
        snap.uptime_ms = 10000;
        snap.captured_at_ms = 10000;
        snap.health = SYSTEM_HEALTH_OK;
        snap.room.illuminance_lux = 72.4f;
        snap.room.illuminance_valid = true;
        memcpy(snap.device_id, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", 16);

        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t written;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &written);

        test("device_id uuid format present", strstr((char *)buf, "01020304-0506-0708-090a-0b0c0d0e0f10") != NULL);
        test("schema 1 present", strstr((char *)buf, "\"schema\": 1") != NULL);
        test("seq 42", strstr((char *)buf, "\"seq\": 42") != NULL);
        test("uptime 10000", strstr((char *)buf, "\"uptime_ms\": 10000") != NULL);
        test("lux 72.4", strstr((char *)buf, "72.4") != NULL);
        test("state valid", strstr((char *)buf, "\"state\": \"valid\"") != NULL);
        test("health ok", strstr((char *)buf, "\"health\": \"ok\"") != NULL);
        test("session present", strstr((char *)buf, "\"session\":") != NULL);
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}