#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "room_sensor_types.h"
#include "network_transport.h"
#include "mqtt_client.h"
#include "communication_port.h"
#include "device_identity.h"

/* ================================================================
   App-level MQTT connection manager + telemetry/command bridge (Phase 18).

   Owns connection lifecycle, reconnect/backoff, telemetry publish and the
   MQTT<->Command bridge for the real production App. It is pure application
   integration:
     - NetworkTransport owns transport mechanism only.
     - MqttClient owns MQTT framing/protocol only.
     - THIS module owns the application reconnect/backoff policy and the App
       connection state machine.
     - Command still owns command parsing + authorization (MQTT only transports
       bytes; it never bypasses authorization).
     - Telemetry is produced by the existing production serializer (no duplicate
       MQTT-specific telemetry representation).

   Deviations/scope:
     - No physical adapter is configured -> module is SAFE_DISABLED (never
       connects, never tight-loops).
     - Offline telemetry is DROPPED, never backlogged.
     - Telemetry: QoS0. Command response: QoS1 (single inflight slot).
     - All buffers compile-time bounded; no dynamic allocation; no sleeps.

   Topic namespace (device-scoped):
     room-sensor/<short-id>/telemetry
     room-sensor/<short-id>/command
     room-sensor/<short-id>/response
     room-sensor/<short-id>/status

   Identity source: the authoritative DeviceIdentity.device_uuid, formatted to the
   short (12-hex) id. Deterministic; does NOT use boot_id.
   ================================================================ */

#define MQTT_APP_TOPIC_MAX       (64U)            /* fits MQTT_MAX_TOPIC_LENGTH */
#define MQTT_APP_PREFIX          "room-sensor/"
#define MQTT_APP_TOPIC_TELEM     "/telemetry"
#define MQTT_APP_TOPIC_CMD       "/command"
#define MQTT_APP_TOPIC_RESP      "/response"
#define MQTT_APP_TOPIC_STATUS    "/status"
#define MQTT_APP_SHORT_ID_LEN    12U              /* 6 bytes -> 12 hex chars */

typedef enum
{
    MQTT_APP_DISABLED = 0,     /* no adapter registered -> never connect */
    MQTT_APP_DISCONNECTED,     /* ready but idle */
    MQTT_APP_CONNECTING,       /* transport connect in progress */
    MQTT_APP_MQTT_CONNECTING,  /* CONNECT sent, awaiting CONNACK */
    MQTT_APP_SUBSCRIBING,      /* awaiting SUBACK on command topic */
    MQTT_APP_ONLINE,           /* connected + subscribed */
    MQTT_APP_BACKOFF           /* terminal failure; waiting backoff before retry */
} MqttAppState;

/* Bounded reconnect backoff schedule (ms): 1s, 2s, 5s, 10s, 30s, 60s (capped,
   then the schedule repeats visiting the same bounded set — never unblinded).
   Steps iterate 0..STEPS-1 and stay capped at the final element. */
#define MQTT_APP_BACKOFF_STEPS     6U
#define MQTT_APP_BACKOFF_SCHEDULE_MS(i)  MqttApp_BackoffScheduleMs(i)

/* Observability counters (bounded, wrap-safe uint32). */
typedef struct
{
    uint32_t connect_attempts;
    uint32_t connect_successes;
    uint32_t connect_failures;
    uint32_t reconnect_backoff_ms;
    uint32_t telemetry_publish_attempts;
    uint32_t telemetry_publish_accepted;
    uint32_t command_messages_received;
    uint32_t command_responses_published;
    uint32_t command_responses_busy_rejected;   /* OPTION-A ingress rejects (Phase 18.2) */
    uint32_t epoch;              /* increments on each terminal disconnect */
} MqttAppStats;

/* Application connection manager (caller statically allocates). */
typedef struct
{
    const NetworkTransportAdapter *adapter;   /* NULL => disabled */
    NetworkEndpoint endpoint;

    NetworkTransport net;                     /* transport instance */
    MqttClient       mqtt;                    /* MQTT client instance */

    /* topic strings, built once at init from device identity */
    char topic_telemetry[MQTT_APP_TOPIC_MAX + 1U];
    char topic_command[MQTT_APP_TOPIC_MAX + 1U];
    char topic_response[MQTT_APP_TOPIC_MAX + 1U];
    char topic_status[MQTT_APP_TOPIC_MAX + 1U];
    char client_id[MQTT_MAX_CLIENT_ID_LENGTH + 1U];   /* stable short id */
    bool topics_built;

    MqttAppState state;
    uint32_t     last_tick_ms;

    /* reconnect/backoff state */
    uint32_t backoff_index;
    uint32_t backoff_until_ms;
    uint8_t  backoff_step;        /* 0..MQTT_APP_BACKOFF_STEPS-1 */

    /* telemetry bridge */
    bool telemetry_pending;       /* one latest snapshot pending if busy */

    /* command response bridge (MqttApp implements CommunicationPort receive) */
    bool response_pending;

    MqttAppStats stats;
    DeviceIdentity identity;
} MqttApp;

#ifdef __cplusplus
extern "C" {
#endif

/* Configure. Copies adapter (may be NULL -> SAFE_DISABLED) and endpoint. */
void MqttApp_Init(MqttApp *app, const NetworkTransportAdapter *adapter,
                  const NetworkEndpoint *endpoint, const DeviceIdentity *identity);

/* Cooperative progress; call from App_Run each tick. Non-blocking. Returns whether
   any progress was made this tick. */
bool MqttApp_Run(MqttApp *app);

/* Publish the production serialized telemetry bytes (QoS0). Drops when offline;
   skips when the single packet/TX slot is busy (fresh state on next period). */
void MqttApp_PublishTelemetry(MqttApp *app, const uint8_t *payload, size_t len);

/* Route a provisioned command response (produced by the Command subsystem) to the
   response topic (QoS1). */
void MqttApp_PublishResponse(MqttApp *app, const uint8_t *payload, size_t len);

MqttAppState MqttApp_GetState(const MqttApp *app);
void MqttApp_GetStats(const MqttApp *app, MqttAppStats *out);

/* CommunicationPort receive/callback used to bridge Command responses into MQTT. */
CommunicationStatus MqttApp_PortSend(void *ctx, const uint8_t *data, size_t size);
bool MqttApp_PortReady(void *ctx);

/* MQTT inbound-publish callback (delivered by the MQTT client). */
bool MqttApp_OnInboundPublish(void *ctx, const MqttInboundPublish *pub);

#ifdef __cplusplus
}
#endif

#endif