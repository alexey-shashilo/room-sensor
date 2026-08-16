#ifndef VIRTUAL_DEVICE_H
#define VIRTUAL_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"

/* Whole-device virtual host runner (Phase 15). Drives the REAL portable core
   (App_Init / App_Run) deterministically under virtual time, exactly like the
   firmware main loop, with all production sensor runtimes / RoomState /
   telemetry running against a scriptable fake I2C bus.

   This is a TEST-ONLY orchestration layer. It does NOT duplicate production
   logic: it only (a) configures the fake hardware per deterministic scenario,
   (b) steps the virtual clock, and (c) calls App_Run once per virtual step.
   The production App/RoomState/telemetry/recovery code is what actually runs. */

/* Runtime size of the real telemetry buffer. */
#define VDEV_TELEMETRY_MAX_SIZE 2048U

typedef struct
{
    FakeI2cBus i2c;
    I2cBus     bus;

    /* Watchdog / reset handles are bound via the host platform globals. */
} VirtualDevice;

/* Reset the entire host-visible device state so a fresh boot can be simulated
   deterministically in-process. Re-initializes the fake I2C bus, the fake
   flash, the fake UID, and the virtual clock. */
void VirtualDevice_Reset(VirtualDevice *dev);

/* Bring all currently-implemented sensors (VEML, display, SCD41, SHT45,
   BMP390, SGP41) to their healthy scripted state so App can reach READY. The
   fake is seeded with realistic, valid register/response vectors. */
void VirtualDevice_InstallHealthySensors(VirtualDevice *dev);

/* Run the device boot (App_Init + App_Run until OPERATIONAL), then step for
   `steps` App_Run calls, advancing virtual time by `step_ms` each step.
   Returns the tick value after the run. */
uint32_t VirtualDevice_RunBoothAndStep(VirtualDevice *dev, uint32_t step_ms, uint32_t steps);

/* Step App_Run once and advance virtual time AFTER the call (like the firmware
   main loop: run then sleep). Equivalent granularity to the production
   CONFIG scheduler cadence (500 ms). */
void VirtualDevice_Step(VirtualDevice *dev, uint32_t step_ms);

/* Wait for the device to reach a given tick deadline by stepping App_Run. */
void VirtualDevice_RunUntil(VirtualDevice *dev, uint32_t target_ms, uint32_t step_ms);

/* Convenience sensor fixtures: compute a valid SGP41 measure response (6 bytes:
   VOC word+CRC, NOx word+CRC) and conditioning response (3 bytes: word+CRC). */
void VDev_Sgp41MeasureResponse(uint16_t voc, uint16_t nox, uint8_t out[6]);
void VDev_Sgp41ConditioningResponse(uint16_t raw, uint8_t out[3]);

/* Compute a valid SHT45 6-byte response for real T/RH (T word+CRC, RH word+CRC,
   high-precision encoding per the SHT4x datasheet). */
void VDev_Sht45Response(float temp_c, float rh_pct, uint8_t out[6]);

/* Update the fake with a fresh valid SHT45 reading. */
void VDev_UpdateSht45(VirtualDevice *dev, float temp_c, float rh_pct);

/* Update the fake with a fresh valid SCD41 measurement (CO2, temp, RH). */
void VDev_UpdateScd41(VirtualDevice *dev, uint16_t co2_ppm, float temp_c, float rh_pct);

/* Update the fake with a fresh valid BMP390 paired sample (pressure Pa via raw
   words, temperature via raw words) using golden fixture CAL1/RAW1 so the real
   compensation yields ~101325 Pa / ~24.5 C. */
void VDev_UpdateBmp390(VirtualDevice *dev);

/* Update the fake with a fresh valid SGP41 measure response. */
void VDev_UpdateSgp41(VirtualDevice *dev, uint16_t voc_raw, uint16_t nox_raw);

/* Bake the currently-scripted sensor responses into the fake fields so the
   scenario can pre-seed them before boot (VEML ALS reg, display present,
   SCD41/SHT45/BMP390/SGP41 valid). The caller still triggers actual reads via
   App stepping. */
void VirtualDevice_PreloadSamples(VirtualDevice *dev);

#endif