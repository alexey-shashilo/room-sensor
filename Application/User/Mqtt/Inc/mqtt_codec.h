#ifndef MQTT_CODEC_H
#define MQTT_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================
   Portable MQTT 3.1.1 codec (Phase 17).

   Byte-level encoding/decoding of the MQTT 3.1.1 packets needed by the
   Room Sensor client. This file is pure mechanism: it neither owns any state
   nor does I/O. It encodes into / decodes from caller-owned buffers with
   compile-time bounds. No dynamic allocation.

   Fixed-header + variable-byte Remaining Length are handled here; higher-level
   packet bodies are decoded by the client against a bounded parser buffer.
   ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compile-time buffer bounds (derived from Room Sensor use case). -------- */
/* Max MQTT topic length. Telemetry topic is short; keep bounded. */
#define MQTT_MAX_TOPIC_LENGTH    64U

/* Max MQTT payload. The production telemetry serializer emits up to
   TELEMETRY_SERIALIZED_MAX_SIZE (2048) bytes; also carries command payloads. */
#define MQTT_MAX_PAYLOAD_SIZE    2048U

/* Max client-id length (MQTT 3.1.1 spec recommends <= 23; allow 64). */
#define MQTT_MAX_CLIENT_ID_LENGTH 64U

/* Whole bounded MQTT packet budget = telemetry payload + topic + framing.
   Worst-case QoS1 PUBLISH: 2 (topic len) + 64 (topic) + 2 (packet id) + 2048
   (payload) + 1 (fixed header) + 2 (remaining-length varint) = 2119. */
#define MQTT_MAX_PACKET_SIZE     2304U

/* One bounded inflight QoS1 transaction. */
#define MQTT_MAX_INFLIGHT_QOS1   1U

/* Max bytes a whole MQTT packet may consume on the wire (fixed+varint only). */
#define MQTT_FIXED_HEADER_MAX    5U

/* ---- MQTT 3.1.1 packet types. ---------------------------------------------- */
#define MQTT_PKT_CONNECT     1U
#define MQTT_PKT_CONNACK     2U
#define MQTT_PKT_PUBLISH     3U
#define MQTT_PKT_PUBACK      4U
#define MQTT_PKT_PUBREC      5U
#define MQTT_PKT_PUBREL      6U
#define MQTT_PKT_PUBCOMP     7U
#define MQTT_PKT_SUBSCRIBE   8U
#define MQTT_PKT_SUBACK      9U
#define MQTT_PKT_UNSUBSCRIBE 10U
#define MQTT_PKT_UNSUBACK    11U
#define MQTT_PKT_PINGREQ     12U
#define MQTT_PKT_PINGRESP    13U
#define MQTT_PKT_DISCONNECT  14U

/* Protocol level for MQTT 3.1.1. */
#define MQTT_PROTOCOL_LEVEL  4U

/* ---- Codec result. ---------------------------------------------------------- */
typedef enum
{
    MQTT_CODEC_OK = 0,
    MQTT_CODEC_WOULD_BLOCK,       /* need more bytes to complete the fixed header */
    MQTT_CODEC_INVALID_ARG,       /* NULL / zero-size / oversized input           */
    MQTT_CODEC_PACKET_TOO_LARGE,  /* remaining length exceeds MQTT_MAX_PACKET_SIZE */
    MQTT_CODEC_MALFORMED          /* illegal flags / type / RL encoding           */
} MqttCodecStatus;

/* Remaining Length encoding (MQTT 3.1.1 variable byte integer, 1..4 bytes). */
#define MQTT_RL_MAX_BYTES 4U

/* ---- Fixed-header decode state (streaming). --------------------------------- */
typedef struct
{
    uint32_t packet_type;      /* 1..14 */
    uint32_t flags;            /* raw flags nibble (validated by caller) */
    uint32_t remaining_length; /* decoded Remaining Length */
    uint32_t fixed_header_len; /* 1 + RL bytes consumed so far */
} MqttFixedHeader;

/* Streaming fixed-header parser core functions (see .c). */
MqttCodecStatus MqttCodec_ReadVarInt(const uint8_t *buf, size_t available,
                                     size_t *consumed, uint32_t *value);
size_t          MqttCodec_WriteVarInt(uint8_t *buf, size_t cap, uint32_t value);

/* Decode the MQTT fixed header (type + flags + remaining length). Feeds
 * `available` bytes; returns WOULD_BLOCK until the full fixed header is present.
 * Validates nothing about the body except Remaining Length <= MQTT_MAX_PACKET_SIZE
 * and RL encoding length <= 4 (a >4-byte continuation is MALFORMED). */
MqttCodecStatus MqttCodec_DecodeFixedHeader(const uint8_t *buf, size_t available,
                                            MqttFixedHeader *out);

/* ---- Encoders. Return total bytes written, or 0 if the frame does not fit. -- */

/* CONNECT (Clean Session=1, no username/password/will). Returns bytes or 0. */
size_t MqttCodec_EncodeConnect(uint8_t *buf, size_t cap,
                               const char *client_id, size_t client_id_len,
                               uint16_t keepalive_s);

/* PUBLISH. qos 0 or 1. qos1 adds a 2-byte packet id. Returns bytes or 0. */
size_t MqttCodec_EncodePublish(uint8_t *buf, size_t cap,
                               const char *topic, size_t topic_len,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t qos, bool retain, uint16_t packet_id);

/* SUBSCRIBE (single topic filter). Returns bytes or 0. */
size_t MqttCodec_EncodeSubscribe(uint8_t *buf, size_t cap,
                                 const char *topic, size_t topic_len,
                                 uint8_t requested_qos, uint16_t packet_id);

/* PINGREQ / DISCONNECT : 2 bytes each. Returns bytes or 0. */
size_t MqttCodec_EncodePingreq(uint8_t *buf, size_t cap);
size_t MqttCodec_EncodeDisconnect(uint8_t *buf, size_t cap);

/* PUBACK for an inbound QoS1 publish : 4 bytes. Returns bytes or 0. */
size_t MqttCodec_EncodePuback(uint8_t *buf, size_t cap, uint16_t packet_id);

/* ---- Decoded inbound packet bodies. ----------------------------------------- */

/* Decoded CONNACK (Remaining Length must be 2). */
typedef struct
{
    bool     session_present;   /* connack[0] bit 0 */
    uint8_t  return_code;       /* connack[1] */
} MqttConnack;

MqttCodecStatus MqttCodec_DecodeConnack(const uint8_t *body, size_t body_len,
                                        MqttConnack *out);

/* Decoded PUBLISH (everything after fixed-header remaining-length). */
typedef struct
{
    const uint8_t *topic;      /* borrows from caller body buffer; valid only
                                  during the call (client copies as needed) */
    uint32_t topic_len;
    uint8_t  qos;
    bool     retain;
    bool     dup;
    uint16_t packet_id;        /* qos>0 */
    const uint8_t *payload;    /* borrows; valid only during the call */
    uint32_t payload_len;
} MqttInboundPublish;

MqttCodecStatus MqttCodec_DecodePublish(uint8_t qos, bool retain, bool dup,
                                        const uint8_t *body, uint32_t body_len,
                                        MqttInboundPublish *out);

/* Decoded PUBACK (Remaining Length must be 2 unless QoS2 upgrades). */
typedef struct
{
    uint16_t packet_id;
} MqttPuback;

MqttCodecStatus MqttCodec_DecodePuback(const uint8_t *body, size_t body_len,
                                       MqttPuback *out);

/* Decoded SUBACK (Remaining Length >= 3; >=1 return code per requested topic). */
typedef struct
{
    uint16_t packet_id;
    uint8_t  return_code;   /* first return code (Phase 17: one requested topic) */
    bool     granted;       /* return_code 0x00 or 0x01 */
} MqttSuback;

MqttCodecStatus MqttCodec_DecodeSuback(const uint8_t *body, size_t body_len,
                                       MqttSuback *out);

#ifdef __cplusplus
}
#endif

#endif