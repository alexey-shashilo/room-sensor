#ifndef FAKE_COMMUNICATION_PORT_H
#define FAKE_COMMUNICATION_PORT_H

#include "communication_port.h"
#include <stdint.h>
#include <stddef.h>

#define FAKE_COMM_MAX_CAPTURED 2048U

typedef struct
{
    CommunicationStatus send_result;
    bool ready;
    int send_call_count;
    uint8_t last_captured[FAKE_COMM_MAX_CAPTURED];
    size_t last_captured_size;
} FakeCommunicationPort;

void FakeComm_Init(FakeCommunicationPort *fake);
void FakeComm_GetPort(CommunicationPort *port, FakeCommunicationPort *fake);

#endif