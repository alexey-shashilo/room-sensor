#ifndef DEVICE_CAPABILITIES_H
#define DEVICE_CAPABILITIES_H

#include <stdbool.h>

typedef struct
{
    bool illuminance;
    bool temperature;
    bool relative_humidity;
    bool pressure;
    bool co2;
    bool voc;
    bool nox;
    bool presence;

    bool display;
    bool persistent_config;
    bool telemetry;
    bool command_control;
    bool watchdog;
    bool self_test;
} DeviceCapabilities;

void DeviceCapabilities_Get(DeviceCapabilities *caps);

#endif