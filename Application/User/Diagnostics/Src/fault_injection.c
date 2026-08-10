#include "fault_injection.h"

#ifdef ROOM_SENSOR_FAULT_INJECTION

static FaultType s_active_fault = FAULT_NONE;

void FaultInjection_Set(FaultType fault)
{
    s_active_fault = fault;
}

void FaultInjection_Clear(void)
{
    s_active_fault = FAULT_NONE;
}

FaultType FaultInjection_Get(void)
{
    return s_active_fault;
}

bool FaultInjection_IsActive(FaultType fault)
{
    return (s_active_fault == fault);
}

#endif