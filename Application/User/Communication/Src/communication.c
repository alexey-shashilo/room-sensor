#include "communication.h"
#include "telemetry_serializer.h"
#include "platform_time.h"
#include <string.h>

static CommunicationPort s_port;
static bool s_port_configured = false;

static TelemetryBuffer s_buffer;
static CommunicationRuntime s_runtime;
static bool s_initialized = false;

static uint8_t s_tx_buffer[TELEMETRY_SERIALIZED_MAX_SIZE];
static uint32_t s_last_send_attempt_ms = 0;

void Communication_Init(void)
{
    memset(&s_buffer, 0, sizeof(s_buffer));
    memset(&s_runtime, 0, sizeof(s_runtime));
    memset(&s_port, 0, sizeof(s_port));
    s_port_configured = false;
    s_last_send_attempt_ms = 0;
    s_runtime.state = COMM_STATE_DISABLED;
    s_initialized = true;
}

void Communication_SetPort(const CommunicationPort *port)
{
    if (port)
    {
        s_port = *port;
        s_port_configured = true;

        if (s_port.is_ready && s_port.is_ready(s_port.context))
            s_runtime.state = COMM_STATE_READY;
        else
            s_runtime.state = COMM_STATE_DISCONNECTED;
    }
    else
    {
        s_port_configured = false;
        s_runtime.state = COMM_STATE_DISABLED;
    }
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
    if (!s_port_configured) return;

    if (s_port.is_ready && !s_port.is_ready(s_port.context))
    {
        s_runtime.state = COMM_STATE_DISCONNECTED;
        return;
    }

    uint32_t now = Platform_GetTickMs();
    if ((now - s_last_send_attempt_ms) < COMM_RETRY_PERIOD_MS)
        return;
    s_last_send_attempt_ms = now;

    size_t written = 0;
    SerializeStatus ss = Telemetry_Serialize(
        &s_buffer.latest,
        s_tx_buffer,
        sizeof(s_tx_buffer),
        &written);

    if (ss != SERIALIZE_OK)
    {
        s_runtime.serialization_failures++;
        return;
    }

    CommunicationStatus cs = s_port.send(s_port.context, s_tx_buffer, written);

    if (cs == COMM_STATUS_OK)
    {
        s_buffer.pending = false;
        s_runtime.send_successes++;
        s_runtime.last_success_ms = now;
        s_runtime.state = COMM_STATE_READY;
    }
    else if (cs == COMM_STATUS_BUSY)
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