#ifndef COMMUNICATION_PORT_H
#define COMMUNICATION_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum
{
    COMM_STATUS_OK = 0,
    COMM_STATUS_BUSY,
    COMM_STATUS_DISCONNECTED,
    COMM_STATUS_ERROR,
    COMM_STATUS_INVALID_ARG
} CommunicationStatus;

typedef struct
{
    void *context;

    CommunicationStatus (*send)(void *context, const uint8_t *data, size_t size);
    bool (*is_ready)(void *context);
} CommunicationPort;

#endif