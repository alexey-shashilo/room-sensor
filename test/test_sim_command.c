#include <stdio.h>
#include <string.h>

#include "app.h"
#include "device_lifecycle.h"
#include "device_identity.h"
#include "self_test.h"
#include "storage.h"
#include "communication.h"
#include "communication_port.h"
#include "command.h"
#include "platform_time.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "virtual_device.h"

/* Command_SetPort is production code (command.c) not declared in the public
   header; it emits Command_Run responses through a port. Declare it here so the
   test can capture software command responses (physical ingress is NOT
   implemented in this phase). */
void Command_SetPort(const CommunicationPort *port);

/* Phase 15 command-path-in-simulation harness.

   Boots the REAL portable core under virtual time, then injects GET_MANIFEST,
   GET_CAPABILITIES and SELF_TEST through the production Command_ProcessInput /
   Command_Run software path. Every response is emitted as:

       RSP <name>
       <response JSON>
       END

   The companion test_command_sim.py parses each response with a REAL JSON
   parser and asserts the expected fields. This proves the software command
   path works inside the whole-device simulation. Physical command ingress
   (UART/MQTT) is NOT implemented. */

static int s_cmd_count = 0;

typedef struct { int id; } CmdCtx;

static CommunicationStatus send(void *context, const uint8_t *data, size_t size)
{
    (void)context;
    printf("RSP %d\n", ++s_cmd_count);
    fwrite(data, 1, size, stdout);
    if (size > 0 && data[size - 1] != '\n')
        printf("\n");
    printf("END\n");
    fflush(stdout);
    return COMM_STATUS_OK;
}

static bool is_ready(void *context)
{
    (void)context;
    return true;
}

static FakeI2cBus s_fake;
static I2cBus s_bus;
static CmdCtx s_cctx;
static CommunicationPort s_cmd_port;
static CommunicationPort s_comm_port;

static void setup_healthy(void)
{
    FakeFlash_Init();
    FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
    FakePlatform_SetTick(0);
    FakeI2cBus_Init(&s_fake);
    s_fake.probe_result = DRIVER_STATUS_OK;
    FakeI2cBus_GetBus(&s_bus, &s_fake);

    static const uint8_t bmp_cal[21] = {
        0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
        0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
    };
    static const uint8_t bmp_pt[6] = {0x5F,0x5A,0x55, 0x5B,0xC9,0xE6};
    FakeI2cBus_SetBmp390Present(&s_fake, (uint16_t)(0x76U << 1), 0x60U, bmp_cal);
    FakeI2cBus_SetBmp390Regs(&s_fake, (uint8_t)(0x20U | 0x40U), 0U, bmp_pt);

    uint8_t sht[6]; VDev_Sht45Response(23.2f, 41.0f, sht);
    FakeI2cBus_SetSht45Response(&s_fake, sht, true);
    uint8_t cond[3], meas[6];
    VDev_Sgp41ConditioningResponse(0x8000U, cond);
    VDev_Sgp41MeasureResponse(30000U, 25000U, meas);
    FakeI2cBus_SetSgp41ConditioningResponse(&s_fake, cond);
    FakeI2cBus_SetSgp41Response(&s_fake, meas, 6U);
    FakeI2cBus_SetPresent(&s_fake, (uint16_t)(0x10U << 1), true);
    FakeI2cBus_SetPresent(&s_fake, (uint16_t)(0x3CU << 1), true);
    FakeI2cBus_SetPresent(&s_fake, (uint16_t)(0x59U << 1), true);

    /* Bind the command response port and the communication port (both held in
       static storage; the production code keeps the pointer across calls). */
    s_cmd_port.context = &s_cctx;
    s_cmd_port.send = send;
    s_cmd_port.is_ready = is_ready;
    Command_SetPort(&s_cmd_port);
    s_comm_port.context = &s_cctx;
    s_comm_port.send = send;
    s_comm_port.is_ready = is_ready;
    Communication_SetPort(&s_comm_port);
}

/* Inject one command through the production software path and pump App_Run. */
static void inject(const char *cmd, CommandSourceTrust trust)
{
    CommandInput in;
    memset(&in, 0, sizeof(in));
    in.data = (const uint8_t *)cmd;
    in.size = strlen(cmd);
    in.trust = trust;
    Command_ProcessInput(&in);
    App_Run();
}

int main(void)
{
    printf("Phase 15 command-path simulation harness\n");

    setup_healthy();
    App_SetI2C(&s_bus);
    if (App_Init() != ROOM_SENSOR_OK)
    {
        printf("App_Init failed\n");
        return 1;
    }
    int guard = 0;
    while (!DeviceLifecycle_IsOperational() && guard < 24)
    {
        App_Run();
        guard++;
    }

    /* Software command path (trusted local, read-only + self-test). */
    inject("{\"id\":1,\"command\":\"GET_MANIFEST\"}", COMMAND_SOURCE_TRUSTED_LOCAL);
    inject("{\"id\":2,\"command\":\"GET_CAPABILITIES\"}", COMMAND_SOURCE_TRUSTED_LOCAL);
    inject("{\"id\":3,\"command\":\"SELF_TEST\"}", COMMAND_SOURCE_TRUSTED_LOCAL);

    printf("commands=%d END_SUITE\n", s_cmd_count);
    return 0;
}