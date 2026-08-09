#include "communication_debug.h"
#include <stdio.h>
#include <string.h>

typedef struct
{
    uint32_t send_count;
} DebugPortContext;

static DebugPortContext s_debug_ctx;

static CommunicationStatus DebugPort_Send(void *context, const uint8_t *data, size_t size)
{
    if ((data == NULL) || (size == 0))
        return COMM_STATUS_INVALID_ARG;

    (void)context;
    s_debug_ctx.send_count++;

    fwrite(data, 1, size, stdout);
    fflush(stdout);

    return COMM_STATUS_OK;
}

static bool DebugPort_IsReady(void *context)
{
    (void)context;
    return true;
}

void CommunicationDebug_Init(CommunicationPort *port)
{
    if (port == NULL) return;

    memset(&s_debug_ctx, 0, sizeof(s_debug_ctx));

    port->context = &s_debug_ctx;
    port->send = DebugPort_Send;
    port->is_ready = DebugPort_IsReady;
}