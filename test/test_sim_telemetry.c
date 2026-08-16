#include <stdio.h>
#include <string.h>

#include "app.h"
#include "device_lifecycle.h"
#include "device_identity.h"
#include "storage.h"
#include "communication.h"
#include "communication_port.h"
#include "platform_time.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "virtual_device.h"

/* Phase 15 telemetry-validation harness.

   Runs a short deterministic healthy whole-device simulation (the REAL portable
   core via App_Init/App_Run), capturing every telemetry frame the production
   Communication layer serializes, and emits each as:

       TEL <n>
       <raw JSON>
       END

   The companion test_telemetry_sim.py parses each frame with a REAL JSON parser
   (json.loads), asserting the schema, no duplicate keys, validity flags match
   sample validity, and the payload never exceeds the configured buffer. This
   proves telemetry produced DURING a whole-device run is valid, not just
   hand-built fixtures. */

static uint32_t s_frame = 0;

typedef struct
{
    int id;
} SendCtx;

static CommunicationStatus send(void *context, const uint8_t *data, size_t size)
{
    (void)context;
    /* Emit the frame with machine-readable delimiters for the Python parser. */
    printf("TEL %lu\n", (unsigned long)(++s_frame));
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

static void capture_port(CommunicationPort *port)
{
    static SendCtx ctx;
    port->context = &ctx;
    port->send = send;
    port->is_ready = is_ready;
}

static FakeI2cBus s_fake;
static I2cBus s_bus;

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
}

static void arm_healthy_samples(void)
{
    FakeI2cBus_SetScd41DataReady(&s_fake, true);
    FakeI2cBus_SetScd41Measurement(&s_fake, 480, FakeI2cBus_TempRaw(23.6f),
                                   FakeI2cBus_RhRaw(41.5f), false,false,false);
    uint8_t s[6]; VDev_Sht45Response(23.3f, 41.2f, s);
    FakeI2cBus_SetSht45Response(&s_fake, s, true);
    uint8_t m[6]; VDev_Sgp41MeasureResponse(31000U, 26000U, m);
    FakeI2cBus_SetSgp41Response(&s_fake, m, 6U);
}

int main(void)
{
    printf("Phase 15 telemetry validation harness\n");

    setup_healthy();
    CommunicationPort cp;
    capture_port(&cp);
    App_SetI2C(&s_bus);
    if (App_Init() != ROOM_SENSOR_OK)
    {
        printf("App_Init failed\n");
        return 1;
    }
    /* Bind the capturing port AFTER App_Init (App installs a stdout debug port
       during init; the production Communication_Run uses whatever port is set). */
    Communication_SetPort(&cp);

    int guard = 0;
    while (guard < 30)
    {
        App_Run();
        guard++;
    }
    arm_healthy_samples();

    /* ~2 minutes of operation, emitting every serialized telemetry frame. */
    for (uint32_t t = 0; t < 120000U; t += 500U)
    {
        App_Run();
        FakePlatform_AdvanceTick(500);
        if (t % 5000U == 0U)
            arm_healthy_samples();
    }

    printf("frames=%lu END_SUITE\n", (unsigned long)s_frame);
    return 0;
}