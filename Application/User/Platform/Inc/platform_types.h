#ifndef PLATFORM_TYPES_H
#define PLATFORM_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum
{
    PLATFORM_OK = 0,
    PLATFORM_TIMEOUT,
    PLATFORM_BUSY,
    PLATFORM_ERROR,
    PLATFORM_UNSUPPORTED,
    PLATFORM_INVALID_ARG
} PlatformStatus;

#endif