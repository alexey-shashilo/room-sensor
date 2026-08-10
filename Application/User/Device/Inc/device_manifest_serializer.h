#ifndef DEVICE_MANIFEST_SERIALIZER_H
#define DEVICE_MANIFEST_SERIALIZER_H

#include <stdint.h>
#include <stddef.h>
#include "device_manifest.h"

#define DEVICE_MANIFEST_SERIALIZED_MAX_SIZE 1024U

typedef enum
{
    MANIFEST_SERIALIZE_OK = 0,
    MANIFEST_SERIALIZE_BUFFER_TOO_SMALL,
    MANIFEST_SERIALIZE_INVALID_ARG
} ManifestSerializeStatus;

ManifestSerializeStatus DeviceManifest_Serialize(
    const DeviceManifest *manifest,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *written);

#endif