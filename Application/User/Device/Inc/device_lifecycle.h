#ifndef DEVICE_LIFECYCLE_H
#define DEVICE_LIFECYCLE_H

#include "room_sensor_types.h"

void         DeviceLifecycle_Init(LifecycleState initial);
LifecycleState DeviceLifecycle_GetState(void);
bool         DeviceLifecycle_CanTransition(LifecycleState from, LifecycleState to);
bool         DeviceLifecycle_TransitionTo(LifecycleState next);
bool         DeviceLifecycle_IsOperational(void);
const char  *DeviceLifecycle_StateStr(LifecycleState state);

#endif