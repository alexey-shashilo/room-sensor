#include <stdio.h>
#include <string.h>

#include "command_parser.h"
#include "command.h"

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(const char *name, int cond)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static int ParseOk(const char *json)
{
    CommandRequest r;
    return CommandParser_Parse((const uint8_t *)json, strlen(json), &r);
}

int main(void)
{
    printf("Command Parser Strict Grammar Tests\n");
    fflush(stdout);

    /* ---- valid base cases ---- */
    printf("\n=== Valid grammar ===\n");
    T("GET_STATUS with id+command", ParseOk("{\"id\":1,\"command\":\"GET_STATUS\"}"));
    T("whitespace tolerated", ParseOk(" { \"id\" : 1 , \"command\" : \"GET_STATUS\" } "));
    T("SET_CONFIG with one field", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":500}"));
    T("REGISTER_DEVICE with installation_id",
      ParseOk("{\"id\":1,\"command\":\"REGISTER_DEVICE\",\"installation_id\":\"11111111111111111111111111111111\"}"));
    T("ASSIGN_LOCATION with three ids",
      ParseOk("{\"id\":1,\"command\":\"ASSIGN_LOCATION\",\"installation_id\":\"11111111111111111111111111111111\",\"building_id\":\"22222222222222222222222222222222\",\"room_id\":\"33333333333333333333333333333333\"}"));

    /* ---- strict object grammar ---- */
    printf("\n=== Strict object grammar ===\n");
    T("trailing comma rejected", !ParseOk("{\"id\":1,\"command\":\"GET_STATUS\",}"));
    T("leading comma rejected", !ParseOk("{,\"id\":1,\"command\":\"GET_STATUS\"}"));
    T("double comma rejected", !ParseOk("{\"id\":1,,\"command\":\"GET_STATUS\"}"));
    T("missing comma rejected", !ParseOk("{\"id\":1\"command\":\"GET_STATUS\"}"));
    T("missing colon rejected", !ParseOk("{\"id\" 1,\"command\":\"GET_STATUS\"}"));
    T("duplicate field rejected", !ParseOk("{\"id\":1,\"id\":2,\"command\":\"GET_STATUS\"}"));
    T("trailing garbage rejected", !ParseOk("{\"id\":1,\"command\":\"GET_STATUS\"} trailing"));
    T("bare object rejected", !ParseOk("{}"));
    T("empty input rejected", !ParseOk(""));

    /* ---- strict JSON number grammar ---- */
    printf("\n=== Number grammar ===\n");
    T("accept 0", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":0}"));
    T("accept -1 float", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":-1}"));
    T("accept 1", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":1}"));
    T("accept 1.0 float", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":1.0}"));
    T("accept 0.5 float", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":0.5}"));
    T("accept 1e3 float", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":1e3}"));
    T("accept -2.5e-4 float", ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":-2.5e-4}"));

    T("reject +1 float", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":+1}"));
    T("reject .5 float", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":.5}"));
    T("reject 1. float", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":1.}"));
    T("reject 01 int", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":01}"));
    T("reject 1e float", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":1e}"));
    T("reject 1e+ float", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":1e+}"));
    T("reject --1 float", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_calibration\":--1}"));

    /* ---- uint field strictness ---- */
    printf("\n=== Uint field strictness ===\n");
    T("uint rejects negative", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":-5}"));
    T("uint rejects plus", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":+5}"));
    T("uint rejects fraction", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":5.0}"));
    T("uint rejects exponent", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":5e2}"));
    T("uint32 overflow rejected", !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\",\"light_period_ms\":4294967296}"));

    /* ---- command-specific argument contracts ---- */
    printf("\n=== Command-specific args ===\n");
    T("GET_STATUS with argument rejected",
      !ParseOk("{\"id\":1,\"command\":\"GET_STATUS\",\"light_period_ms\":500}"));
    T("REGISTER_DEVICE with building_id rejected",
      !ParseOk("{\"id\":1,\"command\":\"REGISTER_DEVICE\",\"installation_id\":\"11111111111111111111111111111111\",\"building_id\":\"22222222222222222222222222222222\"}"));
    T("ASSIGN_LOCATION with config arg rejected",
      !ParseOk("{\"id\":1,\"command\":\"ASSIGN_LOCATION\",\"installation_id\":\"11111111111111111111111111111111\",\"building_id\":\"22222222222222222222222222222222\",\"room_id\":\"33333333333333333333333333333333\",\"light_period_ms\":500}"));
    T("ASSIGN_LOCATION missing room_id rejected",
      !ParseOk("{\"id\":1,\"command\":\"ASSIGN_LOCATION\",\"installation_id\":\"11111111111111111111111111111111\",\"building_id\":\"22222222222222222222222222222222\"}"));
    T("unknown field rejected", !ParseOk("{\"id\":1,\"command\":\"GET_STATUS\",\"bogus\":1}"));

    /* unknown command NAME parses; authorization rejects it (fail closed) */
    {
        CommandRequest r;
        int parsed = CommandParser_Parse(
            (const uint8_t *)"{\"id\":1,\"command\":\"DO_THING\"}",
            strlen("{\"id\":1,\"command\":\"DO_THING\"}"), &r);
        T("unknown command name parses (auth decides)",
          parsed && r.type == COMMAND_UNKNOWN);
    }

    /* ---- empty SET_CONFIG policy ---- */
    printf("\n=== Empty SET_CONFIG ===\n");
    T("empty SET_CONFIG rejected (documented)",
      !ParseOk("{\"id\":1,\"command\":\"SET_CONFIG\"}"));

    /* ---- malformed entity IDs ---- */
    printf("\n=== Entity ID grammar ===\n");
    T("bad hex id rejected",
      !ParseOk("{\"id\":1,\"command\":\"REGISTER_DEVICE\",\"installation_id\":\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"}"));
    T("short hex id rejected",
      !ParseOk("{\"id\":1,\"command\":\"REGISTER_DEVICE\",\"installation_id\":\"1111\"}"));

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}