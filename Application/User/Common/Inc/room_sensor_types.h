#ifndef ROOM_SENSOR_TYPES_H
#define ROOM_SENSOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    /* Semantic note: ROOM_SENSOR_OK means "the runtime started and can run",
       NOT "every subsystem is healthy". A boot with a degraded (but non-fatal)
       persistent-storage or provisioning subsystem still returns OK so that
       sensing continues; detailed subsystem health is exposed via AppStatus
       (App_GetStatus) and the command interface. ROOM_SENSOR_ERROR is reserved
       for conditions that prevent a safe runtime from starting at all (e.g.
       the I2C bus required for drivers is unavailable). */
    ROOM_SENSOR_OK       = 0,
    ROOM_SENSOR_ERROR    = -1,
    ROOM_SENSOR_TIMEOUT  = -2,
    ROOM_SENSOR_BUSY     = -3,
    ROOM_SENSOR_NOT_FOUND = -4
} RoomSensor_Status;

typedef enum
{
    DEVICE_STATE_UNKNOWN = 0,
    DEVICE_STATE_NOT_FOUND,
    DEVICE_STATE_PROBING,
    DEVICE_STATE_INITIALIZING,
    /* SCD41 periodic runtime: periodic measurement issued and the first sample
       is being produced; data-ready being polled. Not present/error states for
       VEML/display, which use PROBING/INITIALIZING instead. */
    DEVICE_STATE_STARTING,
    DEVICE_STATE_WAITING,
    DEVICE_STATE_READY,
    DEVICE_STATE_ERROR,
    DEVICE_STATE_RECOVERING
} DeviceState;

typedef enum
{
    DRIVER_STATUS_OK = 0,
    DRIVER_STATUS_INVALID_ARG,
    DRIVER_STATUS_BUS_ERROR,
    DRIVER_STATUS_TIMEOUT,
    DRIVER_STATUS_NOT_FOUND,
    DRIVER_STATUS_VERIFY_ERROR,
    DRIVER_STATUS_NOT_READY,
    /* A data word failed its CRC-8 validation (Sensirion protocol). No partial
       measurement is committed on a CRC failure. */
    DRIVER_STATUS_CRC_ERROR,
    /* The requested operation is not available on this bus/context. */
    DRIVER_STATUS_NOT_SUPPORTED,
    /* A device-level fault was reported by the addressed sensor's own status/ERR
       register (e.g. fatal / command / configuration error on the BMP390), NOT
       an I2C transport failure (which is DRIVER_STATUS_BUS_ERROR). Contract:
       the device communicated successfully but reported an internal fault. All
       existing drivers test `== DRIVER_STATUS_OK`, so this value is never
       treated as success; it is a recoverable device fault, not a bus error. */
    DRIVER_STATUS_DEVICE_ERROR
} DriverStatus;

#define CONSECUTIVE_ERROR_THRESHOLD 3U

/* Active barometric provider (Phase 17.7B). Exactly ONE barometric sensor is the
   authoritative pressure/temperature source at any instant. RoomState carries
   the provider + generic barometric values as DOMAIN state; it never owns driver
   pointers. Priority: BMP390 valid/present, else BMP380 valid/present, else NONE. */
typedef enum
{
    BAROMETER_PROVIDER_NONE = 0,
    BAROMETER_PROVIDER_BMP390,
    BAROMETER_PROVIDER_BMP380
} BarometerProvider;

typedef struct
{
    DeviceState state;

    uint32_t init_attempts;
    uint32_t init_failures;

    uint32_t operation_successes;
    uint32_t operation_failures;

    uint32_t recovery_count;
    uint32_t consecutive_errors;

    uint32_t last_success_ms;
    uint32_t last_failure_ms;
} DeviceRuntime;

/* ================================================================
   Device Lifecycle — universal state machine for all Climate Hub devices
   ================================================================ */
typedef enum
{
    SYSTEM_HEALTH_BOOTING = 0,
    SYSTEM_HEALTH_OK,
    SYSTEM_HEALTH_DEGRADED,
    SYSTEM_HEALTH_FAULT
} SystemHealthState;

typedef enum
{
    LIFECYCLE_POWER_ON = 0,
    LIFECYCLE_BOOT,
    LIFECYCLE_PLATFORM_INIT,
    LIFECYCLE_LOAD_CONFIGURATION,
    LIFECYCLE_LOAD_IDENTITY,
    LIFECYCLE_CREATE_BOOT_SESSION,
    LIFECYCLE_SELF_TEST,
    LIFECYCLE_PROBE_PERIPHERALS,
    LIFECYCLE_INITIALIZE_DRIVERS,
    LIFECYCLE_RESTORE_RUNTIME,
    LIFECYCLE_READY,
    LIFECYCLE_OPERATIONAL,
    LIFECYCLE_DEGRADED,
    LIFECYCLE_SAFE_MODE
} LifecycleState;

#endif