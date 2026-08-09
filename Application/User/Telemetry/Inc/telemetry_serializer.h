#ifndef TELEMETRY_SERIALIZER_H
#define TELEMETRY_SERIALIZER_H

#include <stdint.h>
#include <stddef.h>
#include "telemetry.h"

#define TELEMETRY_SERIALIZED_MAX_SIZE 1024U

typedef enum
{
    SERIALIZE_OK = 0,
    SERIALIZE_BUFFER_TOO_SMALL,
    SERIALIZE_INVALID_ARG,
    SERIALIZE_ERROR
} SerializeStatus;

SerializeStatus Telemetry_Serialize(
    const TelemetrySnapshot *snapshot,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *written);

#endif