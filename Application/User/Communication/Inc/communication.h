#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include <stdint.h>
#include <stdbool.h>
#include "communication_port.h"
#include "telemetry.h"

typedef enum
{
    COMM_STATE_DISABLED = 0,
    COMM_STATE_DISCONNECTED,
    COMM_STATE_CONNECTING,
    COMM_STATE_READY,
    COMM_STATE_ERROR
} CommunicationState;

typedef struct
{
    CommunicationState state;

    uint32_t send_successes;
    uint32_t send_failures;
    uint32_t last_success_ms;
    uint32_t last_failure_ms;
} CommunicationRuntime;

void Communication_Init(void);
void Communication_SetPort(const CommunicationPort *port);
void Communication_SubmitSnapshot(const TelemetrySnapshot *snapshot);
void Communication_Run(void);
void Communication_GetRuntime(CommunicationRuntime *runtime);

#endif