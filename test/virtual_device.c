#include "virtual_device.h"
#include <string.h>

#include "app.h"
#include "config.h"
#include "device_identity.h"
#include "device_lifecycle.h"
#include "storage.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_communication_port.h"
#include "communication.h"
#include "command.h"
#include "scd41.h"
#include "sht45.h"
#include "sgp41.h"
#include "bmp390.h"

/* Forward declaration: Command_SetPort is implemented in command.c but is not
   (yet) declared in command.h. It is production code; this is a test-side
   extern so the virtual device can capture command responses through a fake
   port (software command path only; physical ingress is a later phase). */
void Command_SetPort(const CommunicationPort *port);

/* Sensirion CRC-8 (poly 0x31, init 0xFF), same as the drivers/fake. Local copy
   so the fixture encodes responses without depending on driver internals. */
static uint8_t sgp41_crc(const uint8_t *p, size_t n)
{
    uint8_t crc = 0xFFU;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (uint8_t b = 0; b < 8U; b++)
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1U) ^ 0x31U) : (uint8_t)(crc << 1U);
    }
    return crc;
}

void VDev_Sgp41MeasureResponse(uint16_t voc, uint16_t nox, uint8_t out[6])
{
    if (out == NULL) return;
    out[0] = (uint8_t)(voc >> 8U); out[1] = (uint8_t)(voc & 0xFFU);
    out[2] = sgp41_crc(&out[0], 2U);
    out[3] = (uint8_t)(nox >> 8U); out[4] = (uint8_t)(nox & 0xFFU);
    out[5] = sgp41_crc(&out[3], 2U);
}

void VDev_Sgp41ConditioningResponse(uint16_t raw, uint8_t out[3])
{
    if (out == NULL) return;
    out[0] = (uint8_t)(raw >> 8U); out[1] = (uint8_t)(raw & 0xFFU);
    out[2] = sgp41_crc(&out[0], 2U);
}

void VDev_Sht45Response(float temp_c, float rh_pct, uint8_t out[6])
{
    if (out == NULL) return;
    uint16_t t = (uint16_t)((temp_c + 45.0f) * 65535.0f / 175.0f);
    uint16_t rh = (uint16_t)(((rh_pct + 6.0f) * 65535.0f / 125.0f));
    out[0] = (uint8_t)(t >> 8U); out[1] = (uint8_t)(t & 0xFFU);
    out[2] = sgp41_crc(&out[0], 2U);
    out[3] = (uint8_t)(rh >> 8U); out[4] = (uint8_t)(rh & 0xFFU);
    out[5] = sgp41_crc(&out[3], 2U);
}

void VirtualDevice_Reset(VirtualDevice *dev)
{
    if (dev == NULL) return;
    memset(dev, 0, sizeof(*dev));
    FakeFlash_Init();
    FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
    FakePlatform_SetTick(0);
    FakeI2cBus_Init(&dev->i2c);
    dev->i2c.probe_result = DRIVER_STATUS_OK;
    FakeI2cBus_GetBus(&dev->bus, &dev->i2c);
}

/* BMP390 golden fixture 1 (CAL1/RAW1): real compensation ~101325 Pa / ~24.5 C. */
static const uint8_t VDEV_BMP_CAL[21] = {
    0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
    0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
};
static const uint8_t VDEV_BMP_PT[6] = {
    0x5F,0x5A,0x55,   /* raw P 0x555A5F */
    0x5B,0xC9,0xE6    /* raw T 0xE6C95B */
};

void VirtualDevice_InstallHealthySensors(VirtualDevice *dev)
{
    if (dev == NULL) return;

    /* VEML + display present at their wire addresses. */
    FakeI2cBus_SetPresent(&dev->i2c, (uint16_t)(0x10U << 1), true);
    FakeI2cBus_SetPresent(&dev->i2c, (uint16_t)(0x3CU << 1), true);

    /* BMP390 present at primary address with the golden calibration. */
    FakeI2cBus_SetBmp390Present(&dev->i2c, (uint16_t)(0x76U << 1), BMP390_CHIP_ID, VDEV_BMP_CAL);
    FakeI2cBus_SetBmp390Regs(&dev->i2c,
                             (uint8_t)(BMP390_STATUS_DRDY_PRESS | BMP390_STATUS_DRDY_TEMP),
                             0U, VDEV_BMP_PT);

    /* SCD41: script a first valid data-ready + measurement. */
    FakeI2cBus_SetScd41DataReady(&dev->i2c, true);
    FakeI2cBus_SetScd41Measurement(&dev->i2c, 450,
                                   FakeI2cBus_TempRaw(23.5f),
                                   FakeI2cBus_RhRaw(42.0f),
                                   false, false, false);

    /* SHT45: a valid high-precision response. */
    uint8_t sht6[6];
    VDev_Sht45Response(23.2f, 41.0f, sht6);
    FakeI2cBus_SetSht45Response(&dev->i2c, sht6, true);

    /* SGP41: present, with valid conditioning + measure responses. */
    uint8_t cond[3];
    uint8_t meas[6];
    VDev_Sgp41ConditioningResponse(0x8000U, cond);
    VDev_Sgp41MeasureResponse(30000U, 25000U, meas);
    FakeI2cBus_SetSgp41ConditioningResponse(&dev->i2c, cond);
    FakeI2cBus_SetSgp41Response(&dev->i2c, meas, 6U);
    FakeI2cBus_SetPresent(&dev->i2c, (uint16_t)(SGP41_I2C_ADDR << 1), true);
}

void VDev_UpdateSht45(VirtualDevice *dev, float temp_c, float rh_pct)
{
    if (dev == NULL) return;
    uint8_t s[6];
    VDev_Sht45Response(temp_c, rh_pct, s);
    FakeI2cBus_SetSht45Response(&dev->i2c, s, true);
}

void VDev_UpdateScd41(VirtualDevice *dev, uint16_t co2_ppm, float temp_c, float rh_pct)
{
    if (dev == NULL) return;
    FakeI2cBus_SetScd41DataReady(&dev->i2c, true);
    FakeI2cBus_SetScd41Measurement(&dev->i2c, co2_ppm,
                                   FakeI2cBus_TempRaw(temp_c),
                                   FakeI2cBus_RhRaw(rh_pct),
                                   false, false, false);
}

void VDev_UpdateBmp390(VirtualDevice *dev)
{
    if (dev == NULL) return;
    FakeI2cBus_SetBmp390Regs(&dev->i2c,
                             (uint8_t)(BMP390_STATUS_DRDY_PRESS | BMP390_STATUS_DRDY_TEMP),
                             0U, VDEV_BMP_PT);
}

void VDev_UpdateSgp41(VirtualDevice *dev, uint16_t voc_raw, uint16_t nox_raw)
{
    if (dev == NULL) return;
    uint8_t meas[6];
    VDev_Sgp41MeasureResponse(voc_raw, nox_raw, meas);
    FakeI2cBus_SetSgp41Response(&dev->i2c, meas, 6U);
}

void VirtualDevice_Step(VirtualDevice *dev, uint32_t step_ms)
{
    if (dev == NULL) return;
    App_Run();
    FakePlatform_AdvanceTick(step_ms);
}

void VirtualDevice_RunUntil(VirtualDevice *dev, uint32_t target_ms, uint32_t step_ms)
{
    if (dev == NULL) return;
    uint32_t now = FakePlatform_GetTick();
    uint32_t guard = 0;
    while (now < target_ms && guard < (target_ms / step_ms) + 16U)
    {
        VirtualDevice_Step(dev, step_ms);
        now = FakePlatform_GetTick();
        guard++;
    }
}

/* Advance the device until DeviceLifecycle is OPERATIONAL, then step `steps`
   App_Run calls advancing virtual time each step. */
uint32_t VirtualDevice_RunBoothAndStep(VirtualDevice *dev, uint32_t step_ms, uint32_t steps)
{
    if (dev == NULL) return 0U;

    App_SetI2C(&dev->bus);
    if (App_Init() != ROOM_SENSOR_OK)
        return 0xFFFFFFFFU;   /* sentinel: App_Init failed */

    /* Drive the full boot lifecycle (each App_Run makes one transition). */
    int guard = 0;
    while (!DeviceLifecycle_IsOperational() && guard < 24)
    {
        App_Run();
        guard++;
    }

    for (uint32_t i = 0; i < steps; i++)
        VirtualDevice_Step(dev, step_ms);

    return FakePlatform_GetTick();
}