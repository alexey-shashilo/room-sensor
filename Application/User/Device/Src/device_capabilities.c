#include "device_capabilities.h"
#include <stddef.h>

void DeviceCapabilities_Get(DeviceCapabilities *caps)
{
    if (caps == NULL) return;

    caps->illuminance = true;
    caps->temperature = true;      /* SHT45 dedicated temperature sensor */
    caps->relative_humidity = true;/* SHT45 humidity channel */
    caps->pressure = true;   /* BMP390 barometric pressure */
    caps->co2 = true;      /* SCD41 CO2 concentration (ppm) */
    caps->voc = false;
    caps->nox = false;
    caps->presence = false;

    caps->display = true;
    caps->persistent_config = true;
    caps->telemetry = true;
    /* command_control documents command-subsystem SUPPORT (parser + dispatcher
       + authorization + response builder all operational). P2-6: it does NOT
       claim an active physical ingress/egress transport. As of this firmware,
       no production UART RX path calls Command_ProcessInput/Command_SetPort, so
       commands cannot yet arrive from the wire; command_control is a capability
       flag, and a physical operator transport is a separate future wiring step
       (out of scope for this remediation — no UART RX is added). */
    caps->command_control = true;
    caps->watchdog = true;
    caps->self_test = true;
}