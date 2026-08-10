#include "firmware_metadata.h"
#include <stddef.h>

void FirmwareMetadata_Get(FirmwareMetadata *meta)
{
    if (meta == NULL) return;

    meta->version_major = ROOM_SENSOR_FW_VERSION_MAJOR;
    meta->version_minor = ROOM_SENSOR_FW_VERSION_MINOR;
    meta->version_patch = ROOM_SENSOR_FW_VERSION_PATCH;
    meta->version_string = ROOM_SENSOR_FW_VERSION_STRING;

#ifdef ROOM_SENSOR_GIT_SHA
    meta->git_commit = ROOM_SENSOR_GIT_SHA;
#else
    meta->git_commit = "unknown";
#endif

#ifdef ROOM_SENSOR_BUILD_TYPE
    meta->build_type = ROOM_SENSOR_BUILD_TYPE;
#else
    meta->build_type = "unknown";
#endif
}