#ifndef FIRMWARE_METADATA_H
#define FIRMWARE_METADATA_H

#include <stdint.h>

#define ROOM_SENSOR_FW_VERSION_MAJOR 0U
#define ROOM_SENSOR_FW_VERSION_MINOR 1U
#define ROOM_SENSOR_FW_VERSION_PATCH 0U
#define ROOM_SENSOR_FW_VERSION_STRING "0.1.0-dev"

#define ROOM_SENSOR_DEVICE_TYPE "room_sensor"
#define ROOM_SENSOR_MODEL      "room-sensor-v1"

typedef struct
{
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;

    const char *version_string;
    const char *git_commit;
    const char *build_type;
} FirmwareMetadata;

void FirmwareMetadata_Get(FirmwareMetadata *meta);

#endif