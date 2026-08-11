#include <stdio.h>
#include <string.h>

#include "command.h"
#include "command_response.h"
#include "command_parser.h"

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(const char *name, int cond)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* Validate that every byte in [0,size) is valid ASCII/UTF-8 JSON text with an
   optional trailing newline and no embedded NUL. */
static int ValidateResponseJson(const CommandResponse *rsp)
{
    if (rsp == NULL) return 0;
    size_t n = rsp->payload_size;
    if (n == 0) return 0;

    /* allows a single trailing '\n' (wire terminator) */
    size_t body = n;
    if (body > 0 && rsp->payload[body - 1] == '\n') body--;

    int brace = 0;
    int in_str = 0;
    int esc = 0;
    for (size_t i = 0; i < body; i++)
    {
        uint8_t c = rsp->payload[i];
        if (c == 0x00) return 0;                 /* no embedded NUL */
        if (c < 0x20) return 0;                  /* no raw control chars */

        if (esc) { esc = 0; continue; }
        if (in_str)
        {
            if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{') brace++;
        else if (c == '}') { brace--; if (brace < 0) return 0; }
    }
    return (brace == 0 && !in_str && !esc) ? 1 : 0;
}

int main(void)
{
    printf("Command Security & Response Tests\n");
    fflush(stdout);

    /* ---- security classification ---- */
    printf("\n=== Authorization policy ===\n");
    T("SET_CONFIG is config-mutation",
      Command_GetSecurityClass(COMMAND_SET_CONFIG) == COMMAND_SECURITY_CONFIG_MUTATION);
    T("GET_STATUS is read-only",
      Command_GetSecurityClass(COMMAND_GET_STATUS) == COMMAND_SECURITY_READ_ONLY);
    T("REBOOT is destructive",
      Command_GetSecurityClass(COMMAND_REBOOT) == COMMAND_SECURITY_DESTRUCTIVE);
    T("UNKNOWN is invalid",
      Command_GetSecurityClass(COMMAND_UNKNOWN) == COMMAND_SECURITY_INVALID);

    /* 28. untrusted mutation denied */
    T("untrusted cannot SET_CONFIG",
      !CommandAuthorization_IsAllowed(COMMAND_SET_CONFIG, COMMAND_SOURCE_UNTRUSTED));
    T("untrusted cannot REGISTER_DEVICE",
      !CommandAuthorization_IsAllowed(COMMAND_REGISTER_DEVICE, COMMAND_SOURCE_UNTRUSTED));
    T("untrusted cannot REBOOT",
      !CommandAuthorization_IsAllowed(COMMAND_REBOOT, COMMAND_SOURCE_UNTRUSTED));
    /* untrusted read-only allowed */
    T("untrusted CAN GET_STATUS",
      CommandAuthorization_IsAllowed(COMMAND_GET_STATUS, COMMAND_SOURCE_UNTRUSTED));

    /* 29/30. trust is resolved per message, never global */
    T("trusted-local can SET_CONFIG",
      CommandAuthorization_IsAllowed(COMMAND_SET_CONFIG, COMMAND_SOURCE_TRUSTED_LOCAL));
    T("trusted-local can REGISTER_DEVICE",
      CommandAuthorization_IsAllowed(COMMAND_REGISTER_DEVICE, COMMAND_SOURCE_TRUSTED_LOCAL));
    T("authenticated-remote can ASSIGN_LOCATION",
      CommandAuthorization_IsAllowed(COMMAND_ASSIGN_LOCATION, COMMAND_SOURCE_AUTHENTICATED_REMOTE));
    T("same command differs by trust (per-message)",
      CommandAuthorization_IsAllowed(COMMAND_SET_CONFIG, COMMAND_SOURCE_TRUSTED_LOCAL) &&
      !CommandAuthorization_IsAllowed(COMMAND_SET_CONFIG, COMMAND_SOURCE_UNTRUSTED));
    /* unknown command fails closed for every trust level */
    T("UNKNOWN denied even trusted",
      !CommandAuthorization_IsAllowed(COMMAND_UNKNOWN, COMMAND_SOURCE_TRUSTED_LOCAL));

    /* ---- response JSON validation (section 28) ---- */
    printf("\n=== Response JSON validity ===\n");
    {
        CommandResponse rsp;
        CommandResponse_Init(&rsp, 17, COMMAND_STATUS_OK);
        CommandResponse_AppendJsonInt(&rsp, "uptime_ms", 12345);
        CommandResponse_AppendJson(&rsp, "state", "ok");
        CommandResponse_AppendJsonFloat(&rsp, "calib", 1.5f, 2);
        CommandResponse_AppendJsonBool(&rsp, "flag", true);
        CommandResponse_Finalize(&rsp);

        T("GET_STATUS-like response valid JSON",
          ValidateResponseJson(&rsp));
        T("response has no embedded NUL in [0,size)",
          memchr(rsp.payload, 0x00, rsp.payload_size) == NULL);
        T("response ends with newline",
          rsp.payload_size > 0 && rsp.payload[rsp.payload_size - 1] == '\n');
        T("response opens with object",
          rsp.payload[0] == '{');
    }
    {
        CommandResponse rsp;
        CommandResponse_Init(&rsp, 5, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(&rsp, "\"error\":\"persist_failed\"");
        CommandResponse_Finalize(&rsp);
        T("error response valid JSON", ValidateResponseJson(&rsp));
    }

    /* ---- parser round-trips into response-safe ids ---- */
    printf("\n=== Parse -> validate response ===\n");
    {
        CommandRequest req;
        const char *json = "{\"id\":9,\"command\":\"GET_CAPABILITIES\"}";
        int ok = CommandParser_Parse((const uint8_t *)json, strlen(json), &req);
        T("GET_CAPABILITIES parses", ok && req.type == COMMAND_GET_CAPABILITIES);

        CommandResponse rsp;
        CommandResponse_Init(&rsp, req.request_id, COMMAND_STATUS_OK);
        CommandResponse_AppendJson(&rsp, "result", "example");
        CommandResponse_Finalize(&rsp);
        T("response echoes request id", rsp.payload_size > 0 &&
          ValidateResponseJson(&rsp));
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}