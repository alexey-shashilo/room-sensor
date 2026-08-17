#include "mqtt_codec.h"
#include "mqtt_utf8.h"
#include <string.h>

/* Portable MQTT 3.1.1 codec (Phase 17). Pure encode/decode, no I/O, no state,
   no dynamic allocation, all callers provide bounded buffers. */

/* ------------------------------------------------------------------ */
/* Variable-byte Remaining Length                                      */
/* ------------------------------------------------------------------ */

MqttCodecStatus MqttCodec_ReadVarInt(const uint8_t *buf, size_t available,
                                     size_t *consumed, uint32_t *value)
{
    if (buf == NULL || value == NULL || consumed == NULL)
        return MQTT_CODEC_INVALID_ARG;
    *consumed = 0U;
    uint32_t multiplier = 1U;
    uint32_t rl = 0U;
    for (uint32_t i = 0U; i < MQTT_RL_MAX_BYTES; i++)
    {
        if (i >= available)
            return MQTT_CODEC_WOULD_BLOCK;   /* need more bytes */
        uint8_t b = buf[*consumed];
        (*consumed)++;
        rl += (uint32_t)(b & 0x7FU) * multiplier;
        if ((b & 0x80U) == 0U)
        {
            *value = rl;
            return MQTT_CODEC_OK;
        }
        multiplier *= 128U;
        if (i == (MQTT_RL_MAX_BYTES - 1U))
            return MQTT_CODEC_MALFORMED;     /* 5th continuation byte */
    }
    return MQTT_CODEC_MALFORMED;
}

size_t MqttCodec_WriteVarInt(uint8_t *buf, size_t cap, uint32_t value)
{
    if (buf == NULL || value > 268435455U || cap == 0U)
        return 0U;
    uint32_t v = value;
    size_t n = 0U;
    do
    {
        if (n >= cap)
            return 0U;
        uint8_t b = (uint8_t)(v % 128U);
        v /= 128U;
        if (v > 0U) b |= 0x80U;
        buf[n++] = b;
    } while (v > 0U);
    return n;
}

MqttCodecStatus MqttCodec_DecodeFixedHeader(const uint8_t *buf, size_t available,
                                            MqttFixedHeader *out)
{
    if (buf == NULL || out == NULL)
        return MQTT_CODEC_INVALID_ARG;
    if (available == 0U)
        return MQTT_CODEC_WOULD_BLOCK;

    uint8_t b0 = buf[0];
    uint32_t type = (uint32_t)(b0 >> 4U);
    uint32_t flags = (uint32_t)(b0 & 0x0FU);
    if (type < 1U || type > 14U)
        return MQTT_CODEC_MALFORMED;

    size_t rl_consumed = 0U;
    size_t rl_buf = available - 1U;   /* bytes after b0 */
    uint32_t rl = 0U;
    MqttCodecStatus rs = MqttCodec_ReadVarInt(buf + 1U, rl_buf, &rl_consumed, &rl);
    if (rs == MQTT_CODEC_WOULD_BLOCK)
        return MQTT_CODEC_WOULD_BLOCK;
    if (rs != MQTT_CODEC_OK)
        return rs;

    /* Fail-closed on oversized packets at the packet boundary. */
    if (rl > MQTT_MAX_PACKET_SIZE)
        return MQTT_CODEC_PACKET_TOO_LARGE;

    out->packet_type = type;
    out->flags = flags;
    out->remaining_length = rl;
    out->fixed_header_len = 1U + (uint32_t)rl_consumed;
    return MQTT_CODEC_OK;
}

/* ------------------------------------------------------------------ */
/* Encoders                                                            */
/* ------------------------------------------------------------------ */

static size_t write_fixed(uint8_t *buf, size_t cap, uint8_t type_flags, uint32_t rl)
{
    if (buf == NULL || cap == 0U)
        return 0U;
    /* compute varint length */
    uint8_t tmp[MQTT_RL_MAX_BYTES];
    size_t nv = MqttCodec_WriteVarInt(tmp, sizeof(tmp), rl);
    if (nv == 0U || (1U + nv) > cap)
        return 0U;
    buf[0] = type_flags;
    memcpy(buf + 1U, tmp, nv);
    return 1U + nv;
}

static size_t be16_put(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v >> 8U);
    out[1] = (uint8_t)(v & 0xFFU);
    return 2U;
}

size_t MqttCodec_EncodeConnect(uint8_t *buf, size_t cap,
                               const char *client_id, size_t client_id_len,
                               uint16_t keepalive_s)
{
    if (buf == NULL || client_id == NULL || client_id_len == 0U ||
        client_id_len > MQTT_MAX_CLIENT_ID_LENGTH)
        return 0U;
    if (!MqttUtf8_Valid((const uint8_t *)client_id, client_id_len))
        return 0U;
    /* Variable header fixed 10 bytes: 2+4 name, 1 level, 1 flags, 2 keepalive.
       Payload: 2 (cid len) + client_id_len. Remaining length = 10 + 2 + cid. */
    uint32_t rl = 10U + 2U + (uint32_t)client_id_len;
    size_t off = write_fixed(buf, cap, (uint8_t)(MQTT_PKT_CONNECT << 4U), rl);
    if (off == 0U)
        return 0U;
    if (off + 10U + 2U + client_id_len > cap)
        return 0U;

    /* Protocol Name "MQTT" (0x00 0x04 'M''Q''T''T') */
    buf[off++] = 0x00U; buf[off++] = 0x04U;
    buf[off++] = 'M'; buf[off++] = 'Q'; buf[off++] = 'T'; buf[off++] = 'T';
    buf[off++] = MQTT_PROTOCOL_LEVEL;          /* Level 4 */
    buf[off++] = 0x02U;                        /* Connect Flags: Clean Session=1 */
    off += be16_put(buf + off, keepalive_s);   /* Keep Alive */
    off += be16_put(buf + off, (uint16_t)client_id_len);
    memcpy(buf + off, client_id, client_id_len);
    off += client_id_len;
    return off;
}

size_t MqttCodec_EncodePublish(uint8_t *buf, size_t cap,
                               const char *topic, size_t topic_len,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t qos, bool retain, uint16_t packet_id)
{
    if (buf == NULL || topic == NULL || topic_len == 0U ||
        topic_len > MQTT_MAX_TOPIC_LENGTH || qos > 1U ||
        payload_len > MQTT_MAX_PAYLOAD_SIZE)
        return 0U;
    if (!MqttUtf8_IsValidTopicName((const uint8_t *)topic, topic_len))
        return 0U;
    uint32_t extra = (qos > 0U) ? 2U : 0U;    /* packet id for qos1 */
    uint32_t rl = 2U + (uint32_t)topic_len + extra + (uint32_t)payload_len;
    /* fixed header flags: DUP=0, QoS=qos, RETAIN=retain */
    uint8_t flags = (uint8_t)((qos << 1U) | (retain ? 1U : 0U));
    size_t off = write_fixed(buf, cap, (uint8_t)((MQTT_PKT_PUBLISH << 4U) | flags), rl);
    if (off == 0U)
        return 0U;
    if (off + rl > cap)
        return 0U;
    off += be16_put(buf + off, (uint16_t)topic_len);
    memcpy(buf + off, topic, topic_len);
    off += topic_len;
    if (qos > 0U)
    {
        off += be16_put(buf + off, packet_id);
    }
    if (payload_len > 0U)
    {
        memcpy(buf + off, payload, payload_len);
        off += payload_len;
    }
    return off;
}

size_t MqttCodec_EncodeSubscribe(uint8_t *buf, size_t cap,
                                 const char *topic, size_t topic_len,
                                 uint8_t requested_qos, uint16_t packet_id)
{
    if (buf == NULL || topic == NULL || topic_len == 0U ||
        topic_len > MQTT_MAX_TOPIC_LENGTH || requested_qos > 1U || packet_id == 0U)
        return 0U;
    if (!MqttUtf8_IsValidTopicFilter((const uint8_t *)topic, topic_len))
        return 0U;
    uint32_t rl = 2U + (uint32_t)topic_len + 1U;   /* pid + filter + qos byte */
    size_t off = write_fixed(buf, cap, (uint8_t)((MQTT_PKT_SUBSCRIBE << 4U) | 0x02U), rl);
    if (off == 0U)
        return 0U;
    if (off + rl > cap)
        return 0U;
    off += be16_put(buf + off, packet_id);
    off += be16_put(buf + off, (uint16_t)topic_len);
    memcpy(buf + off, topic, topic_len);
    off += topic_len;
    buf[off++] = (uint8_t)(requested_qos & 0x01U);   /* requested QoS */
    return off;
}

size_t MqttCodec_EncodePingreq(uint8_t *buf, size_t cap)
{
    if (buf == NULL || cap < 2U)
        return 0U;
    buf[0] = (uint8_t)(MQTT_PKT_PINGREQ << 4U);
    buf[1] = 0x00U;
    return 2U;
}

size_t MqttCodec_EncodeDisconnect(uint8_t *buf, size_t cap)
{
    if (buf == NULL || cap < 2U)
        return 0U;
    buf[0] = (uint8_t)(MQTT_PKT_DISCONNECT << 4U);
    buf[1] = 0x00U;
    return 2U;
}

size_t MqttCodec_EncodePuback(uint8_t *buf, size_t cap, uint16_t packet_id)
{
    if (buf == NULL || cap < 4U || packet_id == 0U)
        return 0U;
    buf[0] = (uint8_t)(MQTT_PKT_PUBACK << 4U);   /* flags 0000 */
    buf[1] = 0x02U;                              /* remaining length = 2 */
    buf[2] = (uint8_t)(packet_id >> 8U);
    buf[3] = (uint8_t)(packet_id & 0xFFU);
    return 4U;
}

/* ------------------------------------------------------------------ */
/* Decoders                                                            */
/* ------------------------------------------------------------------ */

MqttCodecStatus MqttCodec_DecodeConnack(const uint8_t *body, size_t body_len,
                                        MqttConnack *out)
{
    if (body == NULL || out == NULL)
        return MQTT_CODEC_INVALID_ARG;
    if (body_len != 2U)
        return MQTT_CODEC_MALFORMED;
    /* reserved flags: bits 7..1 of byte0 must be 0 */
    if ((body[0] & 0xFEU) != 0U)
        return MQTT_CODEC_MALFORMED;
    out->session_present = (body[0] & 0x01U) != 0U;
    out->return_code = body[1];
    return MQTT_CODEC_OK;
}

static uint16_t be16_read(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8U) | (uint16_t)p[1]);
}

MqttCodecStatus MqttCodec_DecodePublish(uint8_t qos, bool retain, bool dup,
                                        const uint8_t *body, uint32_t body_len,
                                        MqttInboundPublish *out)
{
    if (body == NULL || out == NULL)
        return MQTT_CODEC_INVALID_ARG;
    if (body_len < 2U)
        return MQTT_CODEC_MALFORMED;
    uint16_t topic_len = be16_read(body);
    if (topic_len == 0U || topic_len > MQTT_MAX_TOPIC_LENGTH)
        return MQTT_CODEC_MALFORMED;
    if (body_len < (uint32_t)topic_len + 2U)
        return MQTT_CODEC_MALFORMED;

    uint32_t pos = 2U;
    const uint8_t *topic = body + pos;
    pos += topic_len;
    /* MQTT: a PUBLISH Topic Name is a non-empty UTF-8 string, MUST NOT contain
       wildcard characters '#'/'+'. Reject invalid/inbound-wildcard topics. */
    if (!MqttUtf8_IsValidTopicName(topic, topic_len))
        return MQTT_CODEC_MALFORMED;

    uint16_t packet_id = 0U;
    if (qos > 0U)
    {
        if (body_len < pos + 2U)
            return MQTT_CODEC_MALFORMED;
        packet_id = be16_read(body + pos);
        if (packet_id == 0U)
            return MQTT_CODEC_MALFORMED;
        pos += 2U;
    }

    out->topic = topic;
    out->topic_len = topic_len;
    out->qos = qos;
    out->retain = retain;
    out->dup = dup;
    out->packet_id = packet_id;
    out->payload = body + pos;
    out->payload_len = body_len - pos;
    if (out->payload_len > MQTT_MAX_PAYLOAD_SIZE)
        return MQTT_CODEC_MALFORMED;
    return MQTT_CODEC_OK;
}

MqttCodecStatus MqttCodec_DecodePuback(const uint8_t *body, size_t body_len,
                                       MqttPuback *out)
{
    if (body == NULL || out == NULL)
        return MQTT_CODEC_INVALID_ARG;
    if (body_len != 2U)
        return MQTT_CODEC_MALFORMED;
    out->packet_id = be16_read(body);
    if (out->packet_id == 0U)
        return MQTT_CODEC_MALFORMED;
    return MQTT_CODEC_OK;
}

MqttCodecStatus MqttCodec_DecodeSuback(const uint8_t *body, size_t body_len,
                                       MqttSuback *out)
{
    if (body == NULL || out == NULL)
        return MQTT_CODEC_INVALID_ARG;
    if (body_len < 3U)
        return MQTT_CODEC_MALFORMED;
    out->packet_id = be16_read(body);
    if (out->packet_id == 0U)
        return MQTT_CODEC_MALFORMED;
    out->return_code = body[2];
    out->granted = (out->return_code == 0x00U) || (out->return_code == 0x01U);
    return MQTT_CODEC_OK;
}