#ifndef PLATFORM_UNIQUE_ID_H
#define PLATFORM_UNIQUE_ID_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PLATFORM_UNIQUE_ID_SIZE 12U

bool Platform_GetUniqueId(uint8_t *out, size_t size);

#endif