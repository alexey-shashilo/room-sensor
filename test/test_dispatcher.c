#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "command.h"
#include "command_dispatcher.h"
#include "device_manifest.h"
#include "device_capabilities.h"
#include "self_test.h"
#include "i2c_bus.h"

/* Dispatcher-level security regressions (P1-3, P1-4, P1-4B).

   Emits CASE <id>\n<response-json>\n lines consumed by the companion
   test_dispatcher.py which pipes each blob through a REAL JSON parser
   (json.loads) and asserts semantic values, proving the production dispatcher
   response IS valid JSON and carries the required fields — never a hand-built
   fake response. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(const char *name, int cond)
{
    s_case++;
    if (cond) { s_pass++; printf("PASS %s\n", name); }
    else      { s_fail++; printf("FAIL %s\n", name); }
}

static void emit_case(const char *id, const CommandResponse *rsp)
{
    printf("CASE %s\n", id);
    fwrite(rsp->payload, 1, rsp->payload_size, stdout);
    if (rsp->payload_size > 0 && rsp->payload[rsp->payload_size - 1] != '\n')
        printf("\n");
    printf("END\n");
}

static int contains_byte(const uint8_t *p, size_t n, uint8_t b)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] == b) return 1;
    return 0;
}

int main(void)
{
    printf("Dispatcher security / capability / selftest regressions\n");
    fflush(stdout);

    /* ---- P1-3: GET_MANIFEST poison + authoritative identity ---- */
    printf("\n== P1-3 GET_MANIFEST (uninitialized-identity disclosure) ==\n");
    {
        DeviceManifest m;
        memset(&m, 0xA5, sizeof(m));
        DeviceIdentity auth;
        memset(&auth, 0, sizeof(auth));
        for (int i = 0; i < (int)sizeof(auth.device_uuid); i++)
            auth.device_uuid[i] = (uint8_t)(0x10 + i);
        auth.hardware_revision = 0x55AA;
        DeviceManifest_Build(&m, &auth);
        T("manifest identity == authoritative",
          memcmp(m.identity.device_uuid, auth.device_uuid, sizeof(auth.device_uuid)) == 0 &&
          m.identity.hardware_revision == auth.hardware_revision);
        T("no 0xA5 poison survives Build into identity UUID",
          m.identity.device_uuid[0] == 0x10);

        CommandRequest req; memset(&req, 0, sizeof(req));
        req.type = COMMAND_GET_MANIFEST;
        req.request_id = 1000; req.has_request_id = true;

        DeviceIdentity authoritative;
        memset(&authoritative, 0xBB, sizeof(authoritative));
        CommandServices svc; memset(&svc, 0, sizeof(svc));
        svc.identity = &authoritative;

        CommandResponse rsp; memset(&rsp, 0, sizeof(rsp));
        int ok = CommandDispatcher_Dispatch(&req, &rsp, &svc);
        T("GET_MANIFEST dispatch OK", ok && rsp.status == COMMAND_STATUS_OK);
        T("response contains manifest", rsp.payload_size > 0 &&
          strstr((const char *)rsp.payload, "\"manifest\"") != NULL);
        T("no 0xA5/uninitialized byte in GET_MANIFEST response",
          contains_byte(rsp.payload, rsp.payload_size, (uint8_t)0xA5) == 0);
        T("identity in response is authoritative (bb hex)",
          strstr((const char *)rsp.payload, "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb") != NULL);
        T("runtime identity untouched by read-only GET_MANIFEST",
          authoritative.device_uuid[0] == 0xBB);
        emit_case("m1", &rsp);

        /* NULL authoritative identity: deterministic zero-filled, valid JSON. */
        CommandResponse rsp2; memset(&rsp2, 0, sizeof(rsp2));
        CommandServices nullsvc; memset(&nullsvc, 0, sizeof(nullsvc)); nullsvc.identity = NULL;
        int ok2 = CommandDispatcher_Dispatch(&req, &rsp2, &nullsvc);
        T("GET_MANIFEST NULL identity still OK", ok2 && rsp2.status == COMMAND_STATUS_OK);
        T("NULL-identity manifest has no uninitialized/0xA5 bytes",
          contains_byte(rsp2.payload, rsp2.payload_size, (uint8_t)0xA5) == 0);
        emit_case("nullid-mf", &rsp2);
    }

    /* ---- P1-4: GET_CAPABILITIES derives from canonical ---- */
    printf("\n=== P1-4 GET_CAPABILITIES == canonical ===\n");
    {
        DeviceCapabilities caps; DeviceCapabilities_Get(&caps);
        CommandRequest req; memset(&req, 0, sizeof(req));
        req.type = COMMAND_GET_CAPABILITIES;
        req.request_id = 8; req.has_request_id = true;
        CommandServices svc; memset(&svc, 0, sizeof(svc));
        CommandResponse rsp; memset(&rsp, 0, sizeof(rsp));
        int ok = CommandDispatcher_Dispatch(&req, &rsp, &svc);
        T("GET_CAPABILITIES dispatch OK", ok && rsp.status == COMMAND_STATUS_OK);
        /* The Python mate asserts each field's JSON value == caps. */
        fprintf(stderr, "PRESSURE_EXPECT=%d\r\n", (int)caps.pressure);
        fprintf(stderr, "NOX_EXPECT=%d\r\n", (int)caps.nox);
        emit_case("caps", &rsp);
    }

    /* ---- P1-4B: SELF_TEST response covers current sensor set ---- */
    printf("\n=== P1-4B SELF_TEST response fields ===\n");
    {
        CommandRequest req; memset(&req, 0, sizeof(req));
        req.type = COMMAND_SELF_TEST;
        req.request_id = 9; req.has_request_id = true;
        CommandServices svc; memset(&svc, 0, sizeof(svc));
        SelfTestReport report; SelfTest_Init(&report);
        report.light_sensor = SELF_TEST_PASS;
        report.display = SELF_TEST_PASS;
        report.co2_sensor = SELF_TEST_PASS;
        report.temp_humidity_sensor = SELF_TEST_PASS;
        report.pressure_sensor = SELF_TEST_PASS;
        svc.self_test = &report;
        CommandResponse rsp; memset(&rsp, 0, sizeof(rsp));
        int ok = CommandDispatcher_Dispatch(&req, &rsp, &svc);
        T("SELF_TEST dispatch OK", ok && rsp.status == COMMAND_STATUS_OK);
        T("has light_sensor", strstr((const char *)rsp.payload, "\"light_sensor\"") != NULL);
        T("has display", strstr((const char *)rsp.payload, "\"display\"") != NULL);
        T("has co2_sensor", strstr((const char *)rsp.payload, "\"co2_sensor\"") != NULL);
        T("has temp_humidity_sensor", strstr((const char *)rsp.payload, "\"temp_humidity_sensor\"") != NULL);
        T("has pressure_sensor", strstr((const char *)rsp.payload, "\"pressure_sensor\"") != NULL);
        emit_case("st-result", &rsp);
    }

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    fflush(stdout);
    return s_fail == 0 ? 0 : 1;
}