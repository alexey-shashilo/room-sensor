#ifndef FAULT_INJECTION_H
#define FAULT_INJECTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ROOM_SENSOR_FAULT_INJECTION

typedef enum
{
    FAULT_NONE = 0,

    FAULT_I2C_TIMEOUT,
    FAULT_I2C_BUS_ERROR,

    FAULT_VEML_NOT_FOUND,
    FAULT_VEML_READ_ERROR,

    FAULT_DISPLAY_ERROR,

    FAULT_STORAGE_READ_ERROR,
    FAULT_STORAGE_WRITE_ERROR,

    FAULT_COMM_DISCONNECTED,
    FAULT_COMM_BUSY,
    FAULT_COMM_SEND_ERROR,

    FAULT_IDENTITY_UID_ERROR
} FaultType;

void     FaultInjection_Set(FaultType fault);
void     FaultInjection_Clear(void);
FaultType FaultInjection_Get(void);
bool     FaultInjection_IsActive(FaultType fault);

#else

#define FaultInjection_Set(f)
#define FaultInjection_Clear()
#define FaultInjection_Get() FAULT_NONE
#define FaultInjection_IsActive(f) false

#endif

#endif