#include <stdio.h>
#include <string.h>
#include <math.h>

#include "telemetry.h"
#include "telemetry_serializer.h"
#include "communication.h"
#include "communication_port.h"
#include "fake_communication_port.h"
#include "fake_platform_time.h"
#include "room_state.h"
#include "config.h"

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
    printf("Telemetry / Communication Remediation Tests\n");

    FakePlatform_SetTick(5000);

    /* snapshot creation with explicit input */
    printf("\n=== Snapshot creation ===\n");
    {
        uint8_t dev_id[16] = {0};
        RoomState rs;
        RoomState_Init(&rs);
        RoomState_UpdateIlluminance(&rs, 72.4f, true);

        TelemetrySnapshotInput input;
        input.device_id = dev_id;
        input.room = &rs;
        input.health = SYSTEM_HEALTH_OK;
        input.uptime_ms = 1000;

        TelemetrySnapshot snap;
        int ok = Telemetry_CreateSnapshot(&snap, &input);
        test("snapshot created", ok);
        test("seq = 1", snap.sequence == 1);
        test("uptime matches", snap.uptime_ms == 1000);
        test("captured_at matches", snap.captured_at_ms == 1000);
        test("health matches", snap.health == SYSTEM_HEALTH_OK);
        test("room copied", snap.room.illuminance_lux == 72.4f);

        rs.illuminance_lux = 0.0f;
        test("snapshot isolated from room change", snap.room.illuminance_lux == 72.4f);
    }

    printf("\n=== Sequence increment ===\n");
    {
        TelemetrySnapshot s1, s2;
        uint8_t dev[16] = {0};
        RoomState rs;
        TelemetrySnapshotInput in = { .device_id = dev, .room = &rs, .health = SYSTEM_HEALTH_OK, .uptime_ms = 0 };
        Telemetry_CreateSnapshot(&s1, &in);
        Telemetry_CreateSnapshot(&s2, &in);
test("seq increments", s2.sequence == s1.sequence + 1);
    test("seq > 0", s2.sequence > 0);
    }

    /* NULL arg rejection */
    printf("\n=== NULL args ===\n");
    {
        uint8_t dev[16];
        RoomState rs;
        TelemetrySnapshotInput in = { .device_id = dev, .room = &rs, .health = SYSTEM_HEALTH_OK, .uptime_ms = 0 };
        test("null snapshot", !Telemetry_CreateSnapshot(NULL, &in));
        test("null input", !Telemetry_CreateSnapshot(&(TelemetrySnapshot){0}, NULL));
        test("null device_id", !Telemetry_CreateSnapshot(&(TelemetrySnapshot){0}, &(TelemetrySnapshotInput){.room = &rs}));
        test("null room", !Telemetry_CreateSnapshot(&(TelemetrySnapshot){0}, &(TelemetrySnapshotInput){.device_id = dev}));
    }

    /* valid illuminance */
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
        test("contains schema", strstr((char *)buf, "\"schema\": 2") != NULL);
        test("contains seq 42", strstr((char *)buf, "\"seq\": 42") != NULL);
        test("contains health ok", strstr((char *)buf, "\"health\": \"ok\"") != NULL);
        test("contains value 72.4", strstr((char *)buf, "72.4") != NULL);
        test("state valid", strstr((char *)buf, "\"state\": \"valid\"") != NULL);
        test("NO session field", strstr((char *)buf, "\"session\"") == NULL);
    test("contains boot_id", strstr((char *)buf, "\"boot_id\"") != NULL);
    }

    /* invalid illuminance */
    printf("\n=== Invalid illuminance ===\n");
    {
        TelemetrySnapshot snap = { .sequence = 1, .uptime_ms = 2000, .captured_at_ms = 2000,
                                   .health = SYSTEM_HEALTH_OK, .room = {.illuminance_valid = false} };
        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t w;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &w);
        test("invalid -> no numeric", strstr((char *)buf, "\"value\":") == NULL);
        test("invalid -> state invalid", strstr((char *)buf, "\"state\": \"invalid\"") != NULL);
    }

    /* health strings */
    printf("\n=== Health strings ===\n");
    {
        TelemetrySnapshot snap = { .sequence = 1, .uptime_ms = 100, .captured_at_ms = 100 };
        uint8_t buf[512]; size_t w;
        snap.health = SYSTEM_HEALTH_BOOTING; Telemetry_Serialize(&snap, buf, sizeof(buf), &w);
        test("booting", strstr((char *)buf, "\"health\": \"booting\"") != NULL);
        snap.health = SYSTEM_HEALTH_OK; Telemetry_Serialize(&snap, buf, sizeof(buf), &w);
        test("ok", strstr((char *)buf, "\"health\": \"ok\"") != NULL);
    }

    /* NULL / too-small buffers */
    printf("\n=== Buffer safety ===\n");
    {
        TelemetrySnapshot snap = { .sequence = 1, .uptime_ms = 100, .captured_at_ms = 100, .health = SYSTEM_HEALTH_OK };
        uint8_t tiny[1]; size_t w;
        test("null snapshot", Telemetry_Serialize(NULL, tiny, 1, &w) == SERIALIZE_INVALID_ARG);
        test("null buffer", Telemetry_Serialize(&snap, NULL, 1, &w) == SERIALIZE_INVALID_ARG);
        test("null written", Telemetry_Serialize(&snap, tiny, 1, NULL) == SERIALIZE_INVALID_ARG);
        test("buf size 0", Telemetry_Serialize(&snap, tiny, 0, &w) == SERIALIZE_BUFFER_TOO_SMALL);
        test("buf size 1", Telemetry_Serialize(&snap, tiny, 1, &w) == SERIALIZE_BUFFER_TOO_SMALL);
        test("written=0 on fail", w == 0);
    }

    /* exact-size success */
    printf("\n=== Exact size ===\n");
    {
        TelemetrySnapshot snap = { .sequence = 1, .uptime_ms = 100, .captured_at_ms = 100, .health = SYSTEM_HEALTH_OK };
        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t req;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &req);
size_t exact = req;
    size_t exact_plus = exact + 4;
    uint8_t exact_buf[TELEMETRY_SERIALIZED_MAX_SIZE];
    size_t w2;
    memset(exact_buf, 0, sizeof(exact_buf));
    SerializeStatus s = Telemetry_Serialize(&snap, exact_buf, exact_plus, &w2);
    test("exact+4 succeeds", s == SERIALIZE_OK);
    test("exact+4 written correct", w2 == exact);

        memset(exact_buf, 0xAA, sizeof(exact_buf));
        size_t w3;
        s = Telemetry_Serialize(&snap, exact_buf, exact - 1, &w3);
        test("exact-1 fails", s == SERIALIZE_BUFFER_TOO_SMALL);
        test("written=0 on too-small", w3 == 0);
    }

    /* NaN handling */
    printf("\n=== NaN ===\n");
    {
        TelemetrySnapshot snap = { .sequence = 1, .uptime_ms = 100, .captured_at_ms = 100,
                                   .health = SYSTEM_HEALTH_OK,
                                   .room = { .illuminance_valid = true, .illuminance_lux = 0.0f / 0.0f } };
        uint8_t buf[512]; size_t w;
        Telemetry_Serialize(&snap, buf, sizeof(buf), &w);
        test("NaN -> state invalid", strstr((char *)buf, "\"state\": \"invalid\"") != NULL);
        test("NaN -> no value", strstr((char *)buf, "\"value\":") == NULL);
    }

    /* Communication: port ownership */
    printf("\n=== Port ownership ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake);
        FakeComm_GetPort(&port, &fake);

        Communication_Init();
        Communication_SetPort(&port);

        TelemetrySnapshot s;
        uint8_t dev[16]; RoomState rs;
        TelemetrySnapshotInput in = { .device_id = dev, .room = &rs, .health = SYSTEM_HEALTH_OK, .uptime_ms = 0 };
        Telemetry_CreateSnapshot(&s, &in);

        Communication_SubmitSnapshot(&s);
        Communication_Run();
        test("sent after copy", fake.send_call_count == 1);
    }

    /* disconnected retains pending */
    printf("\n=== Disconnected ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake); fake.ready = false;
        FakeComm_GetPort(&port, &fake);
        Communication_Init();
        Communication_SetPort(&port);

        TelemetrySnapshot s; uint8_t dev[16]; RoomState rs;
        TelemetrySnapshotInput in = { .device_id = dev, .room = &rs, .health = SYSTEM_HEALTH_OK, .uptime_ms = 0 };
        Telemetry_CreateSnapshot(&s, &in);
        Communication_SubmitSnapshot(&s);
        Communication_Run();
        test("not sent disconnected", fake.send_call_count == 0);
    }

    /* BUSY no failure count */
    printf("\n=== BUSY does not increment failure ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake); fake.send_result = COMM_STATUS_BUSY;
        FakeComm_GetPort(&port, &fake);
        Communication_Init();
        Communication_SetPort(&port);

        TelemetrySnapshot s; uint8_t dev[16]; RoomState rs;
        TelemetrySnapshotInput in = { .device_id = dev, .room = &rs, .health = SYSTEM_HEALTH_OK, .uptime_ms = 0 };
        Telemetry_CreateSnapshot(&s, &in);
        Communication_SubmitSnapshot(&s);
        Communication_Run();

        CommunicationRuntime rt;
        Communication_GetRuntime(&rt);
        test("busy does not increment fails", rt.send_failures == 0);
    }

    /* latest-value-wins */
    printf("\n=== Latest value wins ===\n");
    {
        FakeCommunicationPort fake;
        CommunicationPort port;
        FakeComm_Init(&fake);
        FakeComm_GetPort(&port, &fake);
        Communication_Init();
        Communication_SetPort(&port);

        uint8_t dev[16]; RoomState rs;
        TelemetrySnapshotInput in = { .device_id = dev, .room = &rs, .health = SYSTEM_HEALTH_OK, .uptime_ms = 0 };

        TelemetrySnapshot s1, s2;
        in.uptime_ms = 100; Telemetry_CreateSnapshot(&s1, &in);
        in.uptime_ms = 200; Telemetry_CreateSnapshot(&s2, &in);

        Communication_SubmitSnapshot(&s1);
        Communication_SubmitSnapshot(&s2);
        Communication_Run();

        test("latest seq sent", strstr((char *)fake.last_captured, "\"uptime_ms\": 200") != NULL);
    }

    /* golden payload */
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

        test("device_id uuid format", strstr((char *)buf, "01020304-0506-0708-090a-0b0c0d0e0f10") != NULL);
        test("schema 2", strstr((char *)buf, "\"schema\": 2") != NULL);
        test("seq 42", strstr((char *)buf, "\"seq\": 42") != NULL);
        test("uptime 10000", strstr((char *)buf, "\"uptime_ms\": 10000") != NULL);
        test("lux 72.4", strstr((char *)buf, "72.4") != NULL);
        test("no session field", strstr((char *)buf, "\"session\"") == NULL);
        test("boot_id present", strstr((char *)buf, "\"boot_id\"") != NULL);
        test("boot_id format hex", strstr((char *)buf, "\"0000000000000000\"") != NULL);
        test("no trailing comma before close", strstr((char *)buf, ",}") == NULL);
        test("valid JSON close", strstr((char *)buf, "}\n") != NULL);
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}