#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "network_transport.h"
#include "mqtt_codec.h"

/* ================================================================
   Portable MQTT 3.1.1 client foundation (Phase 17).

   This layer owns MQTT framing, protocol state, keepalive, packet IDs and QoS
   semantics. It does NOT own:
     - the byte-stream connection mechanism  (NetworkTransport),
     - reconnect/backoff/network policy      (future ConnectionManager),
     - the application telemetry scheduler   (App),
     - command authorization                 (Command).

   Checks:
     - MQTT_DYNAMIC_ALLOCATION      = NO   (all buffers compile-time bounded)
     - MQTT_UNBOUNDED_QUEUE         = NO   (single inflight QoS1, one outbound
                                            packet buffer, one pending sub)
     - non-blocking: no sleeps, no spins; progress across MqttClient_Run().
     - uses ONLY Platform_GetTickMs() for time (wrap-safe).

   No broker credentials, no TLS, no physical adapter, no persistence.
   ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Client state machine. ------------------------------------------------ */
typedef enum
{
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING_TRANSPORT,   /* NetworkTransport connecting            */
    MQTT_STATE_WAIT_CONNACK,           /* CONNECT sent, awaiting CONNACK         */
    MQTT_STATE_CONNECTED,              /* CONNACK accepted, operations allowed   */
    MQTT_STATE_DISCONNECTING,          /* DISCONNECT sent, awaiting teardown     */
    MQTT_STATE_ERROR                   /* fatal; requires Disconnect+Connect     */
} MqttClientState;

/* ---- MQTT status / error model. ------------------------------------------- */
typedef enum
{
    MQTT_OK = 0,
    MQTT_WOULD_BLOCK,        /* needs another Run/step; no progress yet          */
    MQTT_IN_PROGRESS,        /* connect in flight; call again                    */
    MQTT_INVALID_ARG,        /* NULL / empty / oversized argument (call-local)   */
    MQTT_NOT_CONNECTED,      /* operation requires CONNECTED                     */
    MQTT_BUSY,               /* resource occupied (inflight/buffer); retry later */
    MQTT_PROTOCOL_ERROR,     /* malformed MQTT packet (packet-fatal -> connection-fatal) */
    MQTT_PACKET_TOO_LARGE,   /* inbound packet exceeds MQTT_MAX_PACKET_SIZE       */
    MQTT_TRANSPORT_ERROR,    /* NetworkTransport terminal failure (connection)   */
    MQTT_TIMEOUT,            /* CONNACK / PINGRESP deadline (connection)         */
    MQTT_BROKER_REFUSED      /* CONNACK return code != 0 (connection)            */
} MqttStatus;

/* ---- Connect configuration. ----------------------------------------------- */
typedef struct
{
    const char *client_id;
    size_t      client_id_len;
    uint16_t    keepalive_s;   /* 0 disables keepalive pings                      */
} MqttConnectConfig;

/* ---- Diagnostic counters (bounded). --------------------------------------- */
typedef struct
{
    uint32_t connect_attempts, connect_successes, connect_failures;
    uint32_t tx_packets, rx_packets;
    uint32_t tx_bytes_protocol, rx_bytes_protocol;
    uint32_t protocol_errors, oversized_packets;
    uint32_t publish_qos0, publish_qos1, puback_rx, puback_tx;
    uint32_t subscribe_requests, suback_rx;
    uint32_t pingreq_tx, pingresp_rx;
    uint32_t transport_errors, timeouts;
} MqttStats;

/* ---- Inbound PUBLISH callback. -------------------------------------------- */
/* Called synchronously from MqttClient_Run with a bounded decode. The topic and
   payload borrow the client's parser buffer and are only valid during the call;
   the callback MUST copy anything it needs. Return true to acknowledge a QoS1
   inbound PUBLISH (a PUBACK is sent). Return false to NOT acknowledge. */
typedef bool (*MqttPublishCallback)(void *ctx, const MqttInboundPublish *pub);

/* Concrete MqttClient (callers allocate it statically; no dynamic allocation).
   All connection-scoped state is reset at each connection epoch boundary. */
typedef struct
{
    /* binder / identity */
    NetworkTransport *transport;
    MqttPublishCallback      publish_cb;
    void                    *publish_ctx;
    char       client_id[MQTT_MAX_CLIENT_ID_LENGTH + 1U];
    size_t     client_id_len;
    uint16_t   keepalive_s;

    /* ---- connection-scoped state (cleared at epoch boundary) ---- */
    MqttClientState state;

    uint8_t  tx_buf[MQTT_MAX_PACKET_SIZE];
    uint32_t tx_total, tx_off;
    uint32_t tx_kind;      /* internal TxKind */
    bool     tx_pending;

    uint8_t  rx_buf[MQTT_MAX_PACKET_SIZE];
    uint32_t rx_len, rx_total, rx_fixed_len, rx_type, rx_flags;

    bool     inflight_active;
    uint16_t inflight_packet_id;

    bool     sub_pending;
    uint16_t sub_packet_id;

    bool     ack_pending;
    uint16_t ack_pending_id;

    uint32_t last_activity_ms;
    bool     ping_outstanding;
    uint32_t ping_sent_ms;
    bool     connack_deadline_set;
    uint32_t connack_deadline_ms;

    /* per-client packet id sequences (uint16, never 0; wrap 65535 -> 1). */
    uint16_t next_packet_id;
    uint16_t next_sub_id;

    MqttStats stats;
} MqttClient;

/* ---- Public API. ---------------------------------------------------------- */

/* Bind to a configured+connected-at-call-time NetworkTransport. The transport
   must already exist (Init'ed); MqttClient will Connect/Disconnect/Run it.
   Rejects unsupported config (client_id absent/oversized). */
MqttStatus MqttClient_Init(MqttClient *c, NetworkTransport *transport,
                           const MqttConnectConfig *cfg,
                           MqttPublishCallback publish_cb, void *publish_ctx);

/* Begin a connection: NetworkTransport connect, then CONNECT, WAIT_CONNACK. */
MqttStatus MqttClient_Connect(MqttClient *c);

/* Explicit disconnect: best-effort DISCONNECT (if connected), transport close,
 * clear all connection-scoped state, return to DISCONNECTED. */
MqttStatus MqttClient_Disconnect(MqttClient *c);

/* Cooperative progress: advance transport, drain outbound packet, ingest inbound
 * bytes, drive keepalive. Call frequently from a watchdog-safe loop. */
MqttStatus MqttClient_Run(MqttClient *c);

/* Outbound QoS0 PUBLISH. Completion = packet bytes LOCALLY ACCEPTED into the
 * NetworkTransport TX ring; NOT broker delivery. Returns MQTT_BUSY if a packet
 * is already buffered. */
MqttStatus MqttClient_PublishQos0(MqttClient *c, const char *topic, size_t topic_len,
                                  const uint8_t *payload, size_t payload_len,
                                  bool retain);

/* Outbound QoS1 PUBLISH (single inflight slot). Returns MQTT_BUSY if a QoS1
 * transaction (or a buffered packet) is already in flight. Completes on matching
 * PUBACK. */
MqttStatus MqttClient_PublishQos1(MqttClient *c, const char *topic, size_t topic_len,
                                  const uint8_t *payload, size_t payload_len);

/* One-topic SUBSCRIBE (QoS0 or 1). Completes on matching SUBACK. BUSY if a
 * subscription is already pending. */
MqttStatus MqttClient_Subscribe(MqttClient *c, const char *topic, size_t topic_len,
                                uint8_t requested_qos);

MqttClientState MqttClient_GetState(const MqttClient *c);
void MqttClient_GetStats(const MqttClient *c, MqttStats *out);

/* ---- Config. --------------------------------------------------------------- */
#define MQTT_KEEPALIVE_DEFAULT_S     60U
#define MQTT_CONNACK_TIMEOUT_MS    10000U
#define MQTT_PINGRESP_TIMEOUT_MS   20000U

#ifdef __cplusplus
}
#endif

#endif