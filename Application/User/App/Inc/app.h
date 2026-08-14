#ifndef APP_H
#define APP_H

#include "room_sensor_types.h"
#include "i2c_bus.h"
#include "self_test.h"
#include "platform_reset.h"
#include "storage.h"

#include <stdbool.h>
#include <stdint.h>

/* SystemHealthState is defined in room_sensor_types.h (portable Common) so the
   lower-level Command layer can reference it without depending on App. */

typedef struct
{
    DeviceRuntime light_sensor;
    DeviceRuntime display;
    DeviceRuntime co2_sensor;
    DeviceRuntime temp_humidity_sensor;   /* SHT45 */
    DeviceRuntime pressure_sensor;        /* BMP390 */

    SystemHealthState health;
    ResetCause reset_cause;
    bool watchdog_active;

    SelfTestReport self_test;

    /* Persistent-storage initialization results (exposed for diagnostics).
       false means storage-backed services (provisioning, config, identity
       persistence) are unavailable and writes fail closed. */
    bool storage_initialized;
    bool provisioning_initialized;

    /* Boot persistence health for config and identity. NOT_FOUND = first boot
       (defaults/derived used and persisted once); CORRUPT/IO_ERROR = runtime
       values used but persistent record preserved untouched (degraded). */
    StorageReadStatus config_storage_status;
    StorageReadStatus identity_storage_status;

    /* Redundancy (A/B mirror) health of each persistent record — separate from
       read status. A readable VALID+IO record reads OK but reports DEGRADED_IO. */
    StorageHealth config_storage_health;
    StorageHealth identity_storage_health;
    StorageHealth provisioning_storage_health;

    uint32_t uptime_ms;
} AppStatus;

/* Initializes the runtime and starts sensing.
   Returns ROOM_SENSOR_OK when the runtime started — even if persistent storage
   or provisioning is degraded (sensing continues; writes fail closed). Exact
   per-subsystem health is available via App_GetStatus (AppStatus) and the
   command interface. Returns ROOM_SENSOR_ERROR only when a safe runtime cannot
   start (e.g. no I2C bus). See room_sensor_types.h ROOM_SENSOR_OK semantics. */
RoomSensor_Status App_Init(void);
void               App_Run(void);
void               App_GetStatus(AppStatus *status);
void               App_SetI2C(const I2cBus *bus);
void               App_DoRetry(void);

/* SCD41 SystemHealth contribution predicate: true only when the SCD41 runtime
   is STARTING/WAITING/READY. NOT_FOUND/ERROR/RECOVERING -> false (degrades
   SystemHealth to DEGRADED, never FAULT). Exported for direct unit testing of
   the health mapping. */
bool App_Scd41HealthOk(DeviceState state);

/* SHT45 SystemHealth contribution predicate: true only when the SHT45 runtime is
   STARTING/WAITING/READY. NOT_FOUND/ERROR/RECOVERING -> false (degrades
   SystemHealth to DEGRADED, never FAULT). Exported for direct unit testing. */
bool App_Sht45HealthOk(DeviceState state);

/* BMP390 SystemHealth contribution: true only when STARTING/WAITING/READY.
   NOT_FOUND/ERROR/RECOVERING -> false (degrades health, never FAULT). */
bool App_Bmp390HealthOk(DeviceState state);

#endif