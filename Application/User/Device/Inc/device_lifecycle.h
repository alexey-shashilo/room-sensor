#ifndef DEVICE_LIFECYCLE_H
#define DEVICE_LIFECYCLE_H

#include "room_sensor_types.h"

/* Universal device lifecycle state machine.
   App_Init() sets initial state; App_Run() advances through phases.
   No hidden flags, no duplicated initialization. */

void         DeviceLifecycle_Init(LifecycleState initial);
LifecycleState DeviceLifecycle_GetState(void);
void         DeviceLifecycle_TransitionTo(LifecycleState next);
bool         DeviceLifecycle_IsOperational(void);
const char  *DeviceLifecycle_StateStr(LifecycleState state);

#endif