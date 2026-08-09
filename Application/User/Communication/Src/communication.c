#include "communication.h"
#include "telemetry_serializer.h"
#include "platform_time.h"
#include <string.h>

static const CommunicationPort *s_port = NULL;

static TelemetryBuffer s_buffer;
static CommunicationRuntime s_runtime;
static bool s_initialized = false;

void Communication_Init(void)
{
    memset(&s_buffer, 0, sizeof(s_buffer));
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.state = COMM_STATE_DISCONNECTED;
    s_initialized = true;
}

void Communication_SetPort(const CommunicationPort *port)
{
    s_port = port;
    if (port && port->is_ready && port->is_ready(port->context))
        s_runtime.state = COMM_STATE_READY;
    else if (port)
        s_runtime.state = COMM_STATE_DISCONNECTED;
    else
        s_runtime.state = COMM_STATE_DISABLED;
}

void Communication_SubmitSnapshot(const TelemetrySnapshot *snapshot)
{
    if (!s_initialized || snapshot == NULL) return;

    s_buffer.latest = *snapshot;
    s_buffer.pending = true;
}

void Communication_Run(void)
{
    if (!s_initialized || !s_buffer.pending) return;
    if (s_port == NULL) return;

    if (s_port->is_ready && !s_port->is_ready(s_port->context))
    {
        s_runtime.state = COMM_STATE_DISCONNECTED;
        return;
    }

    uint8_t serialized[TELEMETRY_SERIALIZED_MAX_SIZE];
    size_t written = 0;

    SerializeStatus status = Telemetry_Serialize(
        &s_buffer.latest,
        serialized,
        sizeof(serialized),
        &written);

    if (status != SERIALIZE_OK) return;

    uint32_t now = Platform_GetTickMs();

    CommunicationStatus send_status = s_port->send(s_port->context, serialized, written);

    if (send_status == COMM_STATUS_OK)
    {
        s_buffer.pending = false;
        s_runtime.send_successes++;
        s_runtime.last_success_ms = now;
        s_runtime.state = COMM_STATE_READY;
    }
    else if (send_status == COMM_STATUS_BUSY)
    {
        s_runtime.state = COMM_STATE_READY;
    }
    else
    {
        s_runtime.send_failures++;
        s_runtime.last_failure_ms = now;
        s_runtime.state = COMM_STATE_ERROR;
    }
}

void Communication_GetRuntime(CommunicationRuntime *runtime)
{
    if (runtime) *runtime = s_runtime;
}