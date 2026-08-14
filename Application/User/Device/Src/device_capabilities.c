#include "device_capabilities.h"
#include <stddef.h>

void DeviceCapabilities_Get(DeviceCapabilities *caps)
{
    if (caps == NULL) return;

    caps->illuminance = true;
    caps->temperature = true;      /* SHT45 dedicated temperature sensor */
    caps->relative_humidity = true;/* SHT45 humidity channel */
    caps->pressure = false;
    caps->co2 = true;      /* SCD41 CO2 concentration (ppm) */
    caps->voc = false;
    caps->nox = false;
    caps->presence = false;

    caps->display = true;
    caps->persistent_config = true;
    caps->telemetry = true;
    caps->command_control = true;
    caps->watchdog = true;
    caps->self_test = true;
}