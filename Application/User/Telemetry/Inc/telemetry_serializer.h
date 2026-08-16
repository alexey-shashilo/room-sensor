#ifndef TELEMETRY_SERIALIZER_H
#define TELEMETRY_SERIALIZER_H

#include <stdint.h>
#include <stddef.h>
#include "telemetry.h"

/* Serialized payload budget. Raised from 1024 to 2048 so the full schema at
   capacity — every channel valid at its maximum (incl. the additive SGP41 VOC/
   NOx raw + index fields) — fits with headroom. This is an internal
   allocation/frame-size bound, NOT a wire/JSON schema change: schema stays v5
   and the payload is still length-delimited by the transport. */
#define TELEMETRY_SERIALIZED_MAX_SIZE 2048U

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