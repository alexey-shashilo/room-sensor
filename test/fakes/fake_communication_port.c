#include "fake_communication_port.h"
#include <string.h>

static CommunicationStatus fake_send(void *context, const uint8_t *data, size_t size)
{
    FakeCommunicationPort *f = (FakeCommunicationPort *)context;
    f->send_call_count++;
    if (size > FAKE_COMM_MAX_CAPTURED) size = FAKE_COMM_MAX_CAPTURED;
    memcpy(f->last_captured, data, size);
    f->last_captured_size = size;
    return f->send_result;
}

static bool fake_is_ready(void *context)
{
    FakeCommunicationPort *f = (FakeCommunicationPort *)context;
    return f->ready;
}

void FakeComm_Init(FakeCommunicationPort *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->send_result = COMM_STATUS_OK;
    fake->ready = true;
}

void FakeComm_GetPort(CommunicationPort *port, FakeCommunicationPort *fake)
{
    if (port == NULL) return;
    port->context = fake;
    port->send = fake_send;
    port->is_ready = fake_is_ready;
}