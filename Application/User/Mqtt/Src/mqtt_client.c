#include "mqtt_client.h"
#include "platform_time.h"
#include <string.h>

/* Portable MQTT 3.1.1 client foundation (Phase 17).

   Owns MQTT framing, protocol state, keepalive, packet IDs, and QoS semantics,
   driving a caller-provided NetworkTransport. No reconnect policy, no dynamic
   allocation, no unbounded queues, no sleeps. All time via Platform_GetTickMs()
   with wrap-safe arithmetic.
   ================================================================ */

/* Wrap-safe elapsed helper (repository u32 semantics). */
static bool Elapsed(uint32_t now, uint32_t ref, uint32_t rel_ms)
{
    uint32_t delta = (uint32_t)(now - ref);
    return (delta < 0x80000000U) && (delta >= rel_ms);
}

#define MAX_PACKETS_PER_RUN 4U

/* What the current outbound tx packet was, for completion side-effects. */
typedef enum
{
    TX_NONE = 0,
    TX_CONNECT,
    TX_PINGREQ,
    TX_DISCONNECT,
    TX_PUBLISH_QOS0,
    TX_PUBLISH_QOS1,
    TX_SUBSCRIBE,
    TX_PUBACK
} TxKind;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */

/* Clear ALL connection-scoped state when a connection epoch ends (explicit
 * disconnect, terminal transport error, protocol-fatal error, keepalive timeout,
 * broker refusal / failed CONNECT). Diagnostic counters and persistent
 * configuration (client_id, keepalive) are NOT cleared. This guarantees no
 * per-connection state leaks into the next connection epoch. */
static void ResetConnectionEpoch(MqttClient *c)
{
    c->tx_pending = false;
    c->tx_kind = TX_NONE;
    c->tx_total = c->tx_off = 0U;
    c->rx_len = c->rx_total = c->rx_fixed_len = c->rx_type = c->rx_flags = 0U;
    c->inflight_active = false;
    c->inflight_packet_id = 0U;
    c->sub_pending = false;
    c->sub_packet_id = 0U;
    c->ack_pending = false;
    c->ack_pending_id = 0U;
    c->ping_outstanding = false;
    c->ping_sent_ms = 0U;
    c->connack_deadline_set = false;
    c->connack_deadline_ms = 0U;
}

static void EnterError(MqttClient *c, MqttStatus reason)
{
    /* Connection-scoped state is cleared; diagnostic counters preserved. */
    c->state = MQTT_STATE_ERROR;
    ResetConnectionEpoch(c);

    if (reason == MQTT_TRANSPORT_ERROR) c->stats.transport_errors++;
    else if (reason == MQTT_TIMEOUT) c->stats.timeouts++;
    else if (reason == MQTT_PROTOCOL_ERROR || reason == MQTT_PACKET_TOO_LARGE)
        c->stats.protocol_errors++;
}

/* Drop the current transport connection on a connection-fatal MQTT error. */
static void CloseTransport(MqttClient *c)
{
    if (c->transport != NULL)
        NetworkTransport_Disconnect(c->transport);
}

/* Queue an encoded packet for outbound transmission. If the packet was already
   encoded directly into c->tx_buf, no copy is performed (keeps large packets off
   the stack). */
static void StartTx(MqttClient *c, uint8_t *pkt, uint32_t n, TxKind kind)
{
    if (n > 0U)
    {
        if (pkt != c->tx_buf)
            memcpy(c->tx_buf, pkt, n);
    }
    c->tx_total = n;
    c->tx_off = 0U;
    c->tx_kind = kind;
    c->tx_pending = (n > 0U);
}

/* One NetworkTransport_Send per Run (bounded). Returns status. */
static MqttStatus DrainTx(MqttClient *c)
{
    if (!c->tx_pending)
        return MQTT_OK;
    size_t accepted = 0U;
    NetworkStatus ns = NetworkTransport_Send(c->transport,
                                             c->tx_buf + c->tx_off,
                                             c->tx_total - c->tx_off,
                                             &accepted);
    if (ns == NET_OK)
    {
        c->tx_off += (uint32_t)accepted;
        if (c->tx_off >= c->tx_total)
        {
            /* Packet fully accepted into the transport TX ring (local acceptance,
               NOT broker delivery). Apply per-kind completion side-effects. */
            c->stats.tx_packets++;
            c->stats.tx_bytes_protocol += c->tx_total;
            TxKind k = c->tx_kind;
            c->tx_pending = false;
            c->tx_kind = TX_NONE;
            c->last_activity_ms = Platform_GetTickMs();

            switch (k)
            {
                case TX_CONNECT:
                    c->state = MQTT_STATE_WAIT_CONNACK;
                    c->connack_deadline_ms = (uint32_t)(Platform_GetTickMs() + MQTT_CONNACK_TIMEOUT_MS);
                    c->connack_deadline_set = true;
                    break;
                case TX_PINGREQ:
                    c->ping_outstanding = true;
                    c->ping_sent_ms = Platform_GetTickMs();
                    c->stats.pingreq_tx++;
                    break;
                case TX_DISCONNECT:
                    /* teardown finalized by Run() DISCONNECTING handling */
                    break;
                default:
                    break;   /* publish/subscribe completion tracked via acks */
            }
        }
        return MQTT_OK;
    }
    else if (ns == NET_WOULD_BLOCK)
    {
        return MQTT_OK;   /* 0 accepted; retry next Run */
    }
    /* terminal transport status: connection lost. */
    CloseTransport(c);
    EnterError(c, MQTT_TRANSPORT_ERROR);
    return MQTT_TRANSPORT_ERROR;
}

/* ------------------------------------------------------------------ */
/* Inbound parsing                                                      */

static MqttCodecStatus TryParseHeader(MqttClient *c)
{
    MqttFixedHeader fh;
    MqttCodecStatus rs = MqttCodec_DecodeFixedHeader(c->rx_buf, c->rx_len, &fh);
    if (rs == MQTT_CODEC_WOULD_BLOCK)
    {
        /* wait for more bytes; cap the header scan at MQTT_FIXED_HEADER_MAX. */
        if (c->rx_len >= (uint32_t)MQTT_FIXED_HEADER_MAX)
            return MQTT_CODEC_MALFORMED;   /* >=5 bytes still unresolved: bad RL */
        return MQTT_CODEC_WOULD_BLOCK;
    }
    if (rs != MQTT_CODEC_OK)
        return rs;
    c->rx_fixed_len = fh.fixed_header_len;
    c->rx_type = fh.packet_type;
    c->rx_flags = fh.flags;
    c->rx_total = fh.fixed_header_len + fh.remaining_length;
    return MQTT_CODEC_OK;
}

/* Fast-path validation of the fixed-header flag nibble per packet type.
 * Returns MQTT_OK if acceptable, MQTT_CODEC_MALFORMED otherwise. */
static MqttCodecStatus ValidateFlags(uint32_t type, uint32_t flags)
{
    switch (type)
    {
        case MQTT_PKT_CONNECT:
        case MQTT_PKT_CONNACK:
        case MQTT_PKT_PUBACK:
        case MQTT_PKT_PUBREC:
        case MQTT_PKT_PUBREL:
        case MQTT_PKT_PUBCOMP:
        case MQTT_PKT_SUBSCRIBE:   /* must be 0010 */
        case MQTT_PKT_SUBACK:
        case MQTT_PKT_UNSUBSCRIBE:
        case MQTT_PKT_UNSUBACK:
        case MQTT_PKT_PINGREQ:
        case MQTT_PKT_PINGRESP:
        case MQTT_PKT_DISCONNECT:
            if (type == MQTT_PKT_SUBSCRIBE)
                return (flags == 0x02U) ? MQTT_CODEC_OK : MQTT_CODEC_MALFORMED;
            if (type == MQTT_PKT_PUBREL || type == MQTT_PKT_UNSUBSCRIBE)
                return (flags == 0x02U) ? MQTT_CODEC_OK : MQTT_CODEC_MALFORMED;
            return (flags == 0U) ? MQTT_CODEC_OK : MQTT_CODEC_MALFORMED;
        case MQTT_PKT_PUBLISH:
        {
            uint32_t qos = (flags >> 1U) & 0x03U;
            bool dup = (flags & 0x08U) != 0U;
            /* MQTT 3.1.1: QoS bits == 3 is malformed; DUP must be 0 for QoS0. */
            if (qos == 3U)
                return MQTT_CODEC_MALFORMED;
            if (qos == 0U && dup)
                return MQTT_CODEC_MALFORMED;
            return MQTT_CODEC_OK;
        }
        default:
            return MQTT_CODEC_MALFORMED;
    }
}

static MqttStatus HandleConnack(MqttClient *c)
{
    if (c->rx_flags != 0U)
        return MQTT_PROTOCOL_ERROR;
    MqttConnack ca;
    MqttCodecStatus rs = MqttCodec_DecodeConnack(c->rx_buf + c->rx_fixed_len,
                                                 c->rx_total - c->rx_fixed_len, &ca);
    if (rs != MQTT_CODEC_OK)
        return MQTT_PROTOCOL_ERROR;
    /* Clean Session = 1: sessionPresent must be 0, else protocol violation. */
    if (ca.session_present)
        return MQTT_PROTOCOL_ERROR;
    if (ca.return_code != 0U)
    {
        c->stats.connect_failures++;
        CloseTransport(c);
        EnterError(c, MQTT_BROKER_REFUSED);
        return MQTT_BROKER_REFUSED;
    }
    c->stats.connect_successes++;
    c->state = MQTT_STATE_CONNECTED;
    c->connack_deadline_set = false;
    c->last_activity_ms = Platform_GetTickMs();
    c->stats.rx_packets++;
    return MQTT_OK;
}

static MqttStatus HandleInboundPublish(MqttClient *c)
{
    uint32_t qos = (c->rx_flags >> 1U) & 0x03U;
    bool retain = (c->rx_flags & 0x01U) != 0U;
    bool dup    = (c->rx_flags & 0x08U) != 0U;
    MqttInboundPublish pub;
    MqttCodecStatus rs = MqttCodec_DecodePublish((uint8_t)qos, retain, dup,
                                                 c->rx_buf + c->rx_fixed_len,
                                                 c->rx_total - c->rx_fixed_len, &pub);
    if (rs != MQTT_CODEC_OK)
        return MQTT_PROTOCOL_ERROR;

    bool ack = false;
    if (c->publish_cb != NULL)
        ack = c->publish_cb(c->publish_ctx, &pub);
    c->stats.rx_packets++;

    if (qos == 1U && ack)
    {
        /* Acknowledge the inbound QoS1 publish. If an outbound packet is mid
           drain, defer using the single bounded ack slot (never drops the ack). */
        if (c->tx_pending)
        {
            c->ack_pending = true;
            c->ack_pending_id = pub.packet_id;
        }
        else
        {
            uint8_t pkt[4];
            size_t n = MqttCodec_EncodePuback(pkt, sizeof(pkt), pub.packet_id);
            if (n == 0U)
                return MQTT_PROTOCOL_ERROR;
            StartTx(c, pkt, (uint32_t)n, TX_PUBACK);
            c->stats.puback_tx++;
        }
    }
    else if (qos == 2U)
    {
        return MQTT_PROTOCOL_ERROR;   /* QoS2 unsupported in Phase 17 */
    }
    return MQTT_OK;
}

static MqttStatus HandlePuback(MqttClient *c)
{
    if (c->rx_flags != 0U)
        return MQTT_PROTOCOL_ERROR;
    MqttPuback pa;
    MqttCodecStatus rs = MqttCodec_DecodePuback(c->rx_buf + c->rx_fixed_len,
                                                c->rx_total - c->rx_fixed_len, &pa);
    if (rs != MQTT_CODEC_OK)
        return MQTT_PROTOCOL_ERROR;
    c->stats.rx_packets++;
    if (c->inflight_active && pa.packet_id == c->inflight_packet_id)
    {
        c->inflight_active = false;
        c->stats.puback_rx++;
    }
    /* wrong/unknown packet id: do NOT complete anything (not fatal). */
    return MQTT_OK;
}

static MqttStatus HandleSuback(MqttClient *c)
{
    if (c->rx_flags != 0U)
        return MQTT_PROTOCOL_ERROR;
    MqttSuback sa;
    MqttCodecStatus rs = MqttCodec_DecodeSuback(c->rx_buf + c->rx_fixed_len,
                                                c->rx_total - c->rx_fixed_len, &sa);
    if (rs != MQTT_CODEC_OK)
        return MQTT_PROTOCOL_ERROR;
    c->stats.rx_packets++;
    if (c->sub_pending && sa.packet_id == c->sub_packet_id)
    {
        c->sub_pending = false;
        c->stats.suback_rx++;
    }
    return MQTT_OK;
}

static MqttStatus HandlePingresp(MqttClient *c)
{
    if (c->rx_flags != 0U)
        return MQTT_PROTOCOL_ERROR;
    if (c->rx_total - c->rx_fixed_len != 0U)
        return MQTT_PROTOCOL_ERROR;
    c->stats.rx_packets++;
    if (c->ping_outstanding)
    {
        c->ping_outstanding = false;
        c->stats.pingresp_rx++;
        c->last_activity_ms = Platform_GetTickMs();
    }
    return MQTT_OK;
}

/* Dispatch a complete inbound packet. */
static MqttStatus DispatchPacket(MqttClient *c)
{
    MqttCodecStatus fs = ValidateFlags(c->rx_type, c->rx_flags);
    if (fs != MQTT_CODEC_OK)
        return MQTT_PROTOCOL_ERROR;

    switch (c->rx_type)
    {
        case MQTT_PKT_CONNACK:
            if (c->state != MQTT_STATE_WAIT_CONNACK)
                return MQTT_PROTOCOL_ERROR;   /* unexpected CONNACK */
            return HandleConnack(c);

        case MQTT_PKT_PUBLISH:
            return HandleInboundPublish(c);

        case MQTT_PKT_PUBACK:
            if (c->state != MQTT_STATE_CONNECTED)
                return MQTT_PROTOCOL_ERROR;
            return HandlePuback(c);

        case MQTT_PKT_SUBACK:
            if (c->state != MQTT_STATE_CONNECTED)
                return MQTT_PROTOCOL_ERROR;
            return HandleSuback(c);

        case MQTT_PKT_PINGRESP:
            if (c->state != MQTT_STATE_CONNECTED)
                return MQTT_PROTOCOL_ERROR;
            return HandlePingresp(c);

        case MQTT_PKT_PUBREC:
        case MQTT_PKT_PUBREL:
        case MQTT_PKT_PUBCOMP:
            return MQTT_PROTOCOL_ERROR;       /* QoS2 unsupported*/

        default:
            /* Any other broker->client packet is a protocol violation. */
            return MQTT_PROTOCOL_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Ingest: pull bytes from the transport into a bounded packet buffer. */

static MqttStatus Ingest(MqttClient *c)
{
    for (uint32_t p = 0U; p < MAX_PACKETS_PER_RUN; p++)
    {
        if (c->rx_len == 0U || c->rx_total == 0U)
        {
            /* Need (fixed header) bytes first. */
            if (c->rx_len == 0U)
            {
                /* fetch up to header budget */
                size_t want0 = MQTT_FIXED_HEADER_MAX;
                size_t got = 0U;
                NetworkStatus ns = NetworkTransport_Receive(c->transport, c->rx_buf,
                                                            want0, &got);
                if (ns == NET_REMOTE_CLOSED || ns == NET_TRANSPORT_ERROR)
                {
                    CloseTransport(c);
                    EnterError(c, MQTT_TRANSPORT_ERROR);
                    return MQTT_TRANSPORT_ERROR;
                }
                if (ns == NET_WOULD_BLOCK || got == 0U)
                    return MQTT_WOULD_BLOCK;
                c->rx_len = (uint32_t)got;
            }
            MqttCodecStatus rs = TryParseHeader(c);
            if (rs == MQTT_CODEC_WOULD_BLOCK)
            {
                /* fetch more header bytes */
                size_t got = 0U;
                uint8_t *dst = c->rx_buf + c->rx_len;
                size_t space = MQTT_FIXED_HEADER_MAX - c->rx_len;
                NetworkStatus ns = NetworkTransport_Receive(c->transport, dst, space, &got);
                if (ns == NET_REMOTE_CLOSED || ns == NET_TRANSPORT_ERROR)
                {
                    CloseTransport(c);
                    EnterError(c, MQTT_TRANSPORT_ERROR);
                    return MQTT_TRANSPORT_ERROR;
                }
                if (ns == NET_WOULD_BLOCK || got == 0U)
                    return MQTT_WOULD_BLOCK;
                c->rx_len += (uint32_t)got;
                rs = TryParseHeader(c);
                if (rs == MQTT_CODEC_WOULD_BLOCK)
                    continue;   /* still need more header bytes; loop to fetch more */
            }
            if (rs == MQTT_CODEC_PACKET_TOO_LARGE)
            {
                c->stats.oversized_packets++;
                CloseTransport(c);
                EnterError(c, MQTT_PACKET_TOO_LARGE);
                return MQTT_PACKET_TOO_LARGE;
            }
            if (rs != MQTT_CODEC_OK)
            {
                CloseTransport(c);
                EnterError(c, MQTT_PROTOCOL_ERROR);
                return MQTT_PROTOCOL_ERROR;
            }
            /* fall through to body accumulation */
        }

        /* Now header is known: accumulate body bytes up to rx_total. */
        if (c->rx_len < c->rx_total)
        {
            size_t want = c->rx_total - c->rx_len;
            size_t got = 0U;
            NetworkStatus ns = NetworkTransport_Receive(c->transport,
                                                        c->rx_buf + c->rx_len,
                                                        want, &got);
            if (ns == NET_REMOTE_CLOSED || ns == NET_TRANSPORT_ERROR)
            {
                CloseTransport(c);
                EnterError(c, MQTT_TRANSPORT_ERROR);
                return MQTT_TRANSPORT_ERROR;
            }
            if (ns == NET_WOULD_BLOCK || got == 0U)
                return MQTT_WOULD_BLOCK;
            c->rx_len += (uint32_t)got;
            if (c->rx_len < c->rx_total)
                return MQTT_WOULD_BLOCK;   /* wait for the rest */
        }

        /* Full packet available. */
        MqttStatus ds = DispatchPacket(c);
        if (ds != MQTT_OK)
        {
            if (ds == MQTT_PROTOCOL_ERROR || ds == MQTT_PACKET_TOO_LARGE)
            {
                CloseTransport(c);
                EnterError(c, ds);
            }
            return ds;
        }
        /* reset parser for next packet */
        c->rx_len = 0U;
        c->rx_total = 0U;
        c->rx_fixed_len = 0U;
        c->rx_type = 0U;
        c->rx_flags = 0U;
    }
    return MQTT_OK;
}

/* ------------------------------------------------------------------ */
/* Keepalive                                                            */

static MqttStatus RunKeepalive(MqttClient *c)
{
    uint32_t now = Platform_GetTickMs();
    if (c->keepalive_s == 0U)
        return MQTT_OK;

    if (c->ping_outstanding)
    {
        if (Elapsed(now, c->ping_sent_ms, MQTT_PINGRESP_TIMEOUT_MS))
        {
            CloseTransport(c);
            EnterError(c, MQTT_TIMEOUT);
            return MQTT_TIMEOUT;
        }
    }
    else if (Elapsed(now, c->last_activity_ms, (uint32_t)c->keepalive_s * 1000U))
    {
        /* No outbound packet within the keepalive window: send PINGREQ. */
        uint8_t pkt[2];
        size_t n = MqttCodec_EncodePingreq(pkt, sizeof(pkt));
        if (n == 0U)
            return MQTT_PROTOCOL_ERROR;
        if (c->tx_pending)
            return MQTT_OK;   /* ping deferred while another packet drains */
        StartTx(c, pkt, (uint32_t)n, TX_PINGREQ);
    }
    return MQTT_OK;
}

static uint16_t NextPacketId(MqttClient *c)
{
    uint16_t id = c->next_packet_id;
    c->next_packet_id = (c->next_packet_id == 0xFFFFU) ? 1U : (uint16_t)(c->next_packet_id + 1U);
    return id;
}

/* ------------------------------------------------------------------ */
/* Public API                                                            */

MqttStatus MqttClient_Init(MqttClient *c, NetworkTransport *transport,
                           const MqttConnectConfig *cfg,
                           MqttPublishCallback publish_cb, void *publish_ctx)
{
    if (c == NULL || transport == NULL || cfg == NULL || cfg->client_id == NULL ||
        cfg->client_id_len == 0U || cfg->client_id_len > MQTT_MAX_CLIENT_ID_LENGTH)
        return MQTT_INVALID_ARG;

    memset(c, 0, sizeof(*c));
    c->transport = transport;
    c->publish_cb = publish_cb;
    c->publish_ctx = publish_ctx;
    c->client_id_len = cfg->client_id_len;
    memcpy(c->client_id, cfg->client_id, cfg->client_id_len);
    c->client_id[cfg->client_id_len] = '\0';
    c->keepalive_s = cfg->keepalive_s;
    c->next_packet_id = 1U;
    c->next_sub_id = 1U;
    c->state = MQTT_STATE_DISCONNECTED;
    return MQTT_OK;
}

MqttStatus MqttClient_Connect(MqttClient *c)
{
    if (c == NULL)
        return MQTT_INVALID_ARG;
    if (c->state != MQTT_STATE_DISCONNECTED)
        return MQTT_BUSY;

    NetworkStatus ns = NetworkTransport_Connect(c->transport);
    if (ns != NET_OK && ns != NET_IN_PROGRESS)
    {
        c->stats.connect_failures++;
        c->state = MQTT_STATE_ERROR;
        return MQTT_TRANSPORT_ERROR;
    }
    c->state = MQTT_STATE_CONNECTING_TRANSPORT;
    c->stats.connect_attempts++;
    return MQTT_IN_PROGRESS;
}

MqttStatus MqttClient_Run(MqttClient *c)
{
    if (c == NULL)
        return MQTT_INVALID_ARG;

    /* 1. Always advance the transport: drive connect progress, drain the
       transport's own TX ring (bytes MQTT previously handed over via local
       acceptance), and fetch inbound bytes from the adapter into the transport
       RX ring. This must happen EVERY Run so CONNACK/PUBACK/SUBACK/PINGRESP and
       inbound PUBLISH bytes actually flow after the transport is connected. */
    NetworkStatus nr = NetworkTransport_Run(c->transport);
    if (nr == NET_TRANSPORT_ERROR)
    {
        CloseTransport(c);
        EnterError(c, MQTT_TRANSPORT_ERROR);
        return MQTT_TRANSPORT_ERROR;
    }
    if (nr == NET_REMOTE_CLOSED)
    {
        /* Transport observed EOF (CLOSING). Falls through: Ingest will surface the
           remote close as it drains. */
    }

    if (c->state == MQTT_STATE_CONNECTING_TRANSPORT)
    {
        if (nr == NET_IN_PROGRESS)
            return MQTT_IN_PROGRESS;
        if (nr != NET_OK)
        {
            /* connect failed / closed at the transport level. */
            c->stats.connect_failures++;
            CloseTransport(c);
            EnterError(c, (nr == NET_TIMEOUT) ? MQTT_TIMEOUT : MQTT_TRANSPORT_ERROR);
            return (nr == NET_TIMEOUT) ? MQTT_TIMEOUT : MQTT_TRANSPORT_ERROR;
        }
        /* Transport is CONNECTED. Build + queue CONNECT (encode into tx_buf to
           avoid a large stack frame). */
        size_t n = MqttCodec_EncodeConnect(c->tx_buf, sizeof(c->tx_buf),
                                           c->client_id, c->client_id_len,
                                           c->keepalive_s);
        if (n == 0U)
        {
            CloseTransport(c);
            EnterError(c, MQTT_PROTOCOL_ERROR);
            return MQTT_PROTOCOL_ERROR;
        }
        StartTx(c, c->tx_buf, (uint32_t)n, TX_CONNECT);
        c->state = MQTT_STATE_WAIT_CONNACK;
        c->connack_deadline_ms = (uint32_t)(Platform_GetTickMs() + MQTT_CONNACK_TIMEOUT_MS);
        c->connack_deadline_set = true;
    }

    /* 2. Drain outbound packet toward the transport (local acceptance). */
    {
        MqttStatus ds = DrainTx(c);
        if (ds != MQTT_OK)
            return ds;
    }

    /* 3. Per-state processing + inbound. */
    switch (c->state)
    {
        case MQTT_STATE_WAIT_CONNACK:
        {
            /* Detect transport terminal error / remote close during wait. */
            NetworkState ts2 = NetworkTransport_GetState(c->transport);
            if (ts2 == NET_STATE_ERROR || ts2 == NET_STATE_CLOSING)
            {
                c->stats.connect_failures++;
                CloseTransport(c);
                EnterError(c, MQTT_TRANSPORT_ERROR);
                return MQTT_TRANSPORT_ERROR;
            }
            uint32_t now = Platform_GetTickMs();
            if (c->connack_deadline_set &&
                Elapsed(now, c->connack_deadline_ms, MQTT_CONNACK_TIMEOUT_MS))
            {
                c->stats.timeouts++;
                CloseTransport(c);
                EnterError(c, MQTT_TIMEOUT);
                return MQTT_TIMEOUT;
            }
            MqttStatus is = Ingest(c);
            if (is != MQTT_WOULD_BLOCK && is != MQTT_OK)
                return is;   /* includes CONNACK -> CONNECTED path */
            break;
        }

        case MQTT_STATE_CONNECTED:
        {
            /* Flush a deferred inbound-QoS1 PUBACK now that TX is free. */
            if (c->ack_pending && !c->tx_pending)
            {
                uint8_t pkt[4];
                size_t n = MqttCodec_EncodePuback(pkt, sizeof(pkt), c->ack_pending_id);
                if (n == 0U)
                    return MQTT_PROTOCOL_ERROR;
                StartTx(c, pkt, (uint32_t)n, TX_PUBACK);
                c->stats.puback_tx++;
                c->ack_pending = false;
            }
            MqttStatus is = Ingest(c);
            if (is != MQTT_WOULD_BLOCK && is != MQTT_OK)
                return is;
            MqttStatus ks = RunKeepalive(c);
            if (ks != MQTT_OK)
                return ks;
            break;
        }

        case MQTT_STATE_DISCONNECTING:
        {
            /* Drain the DISCONNECT then finish. */
            if (c->tx_pending)
            {
                MqttStatus ds = DrainTx(c);
                if (ds != MQTT_OK)
                    return ds;
            }
            CloseTransport(c);
            ResetConnectionEpoch(c);
            c->state = MQTT_STATE_DISCONNECTED;
            break;
        }

        default:
            break;
    }
    return MQTT_OK;
}

MqttStatus MqttClient_Disconnect(MqttClient *c)
{
    if (c == NULL)
        return MQTT_INVALID_ARG;
    if (c->state == MQTT_STATE_DISCONNECTED)
        return MQTT_OK;

    if (c->state == MQTT_STATE_CONNECTED)
    {
        /* Best-effort DISCONNECT packet, then teardown. */
        uint8_t pkt[2];
        size_t n = MqttCodec_EncodeDisconnect(pkt, sizeof(pkt));
        StartTx(c, pkt, (uint32_t)n, TX_DISCONNECT);
        c->state = MQTT_STATE_DISCONNECTING;
        return MQTT_OK;
    }

    /* From any other state: teardown immediately. */
    CloseTransport(c);
    ResetConnectionEpoch(c);
    c->state = MQTT_STATE_DISCONNECTED;
    return MQTT_OK;
}

MqttStatus MqttClient_PublishQos0(MqttClient *c, const char *topic, size_t topic_len,
                                  const uint8_t *payload, size_t payload_len,
                                  bool retain)
{
    if (c == NULL)
        return MQTT_INVALID_ARG;
    if (c->state != MQTT_STATE_CONNECTED)
        return MQTT_NOT_CONNECTED;
    if (topic == NULL || topic_len == 0U || topic_len > MQTT_MAX_TOPIC_LENGTH ||
        (payload == NULL && payload_len != 0U) || payload_len > MQTT_MAX_PAYLOAD_SIZE)
        return MQTT_INVALID_ARG;
    if (c->tx_pending)
        return MQTT_BUSY;

    size_t n = MqttCodec_EncodePublish(c->tx_buf, sizeof(c->tx_buf), topic, topic_len,
                                       payload, payload_len, 0, retain, 0U);
    if (n == 0U)
        return MQTT_PACKET_TOO_LARGE;
    StartTx(c, c->tx_buf, (uint32_t)n, TX_PUBLISH_QOS0);
    c->stats.publish_qos0++;
    c->last_activity_ms = Platform_GetTickMs();
    return MQTT_OK;
}

MqttStatus MqttClient_PublishQos1(MqttClient *c, const char *topic, size_t topic_len,
                                  const uint8_t *payload, size_t payload_len)
{
    if (c == NULL)
        return MQTT_INVALID_ARG;
    if (c->state != MQTT_STATE_CONNECTED)
        return MQTT_NOT_CONNECTED;
    if (topic == NULL || topic_len == 0U || topic_len > MQTT_MAX_TOPIC_LENGTH ||
        (payload == NULL && payload_len != 0U) || payload_len > MQTT_MAX_PAYLOAD_SIZE)
        return MQTT_INVALID_ARG;
    if (c->tx_pending || c->inflight_active)
        return MQTT_BUSY;

    uint16_t id = NextPacketId(c);

    size_t n = MqttCodec_EncodePublish(c->tx_buf, sizeof(c->tx_buf), topic, topic_len,
                                       payload, payload_len, 1, false, id);
    if (n == 0U)
        return MQTT_PACKET_TOO_LARGE;
    StartTx(c, c->tx_buf, (uint32_t)n, TX_PUBLISH_QOS1);
    c->inflight_active = true;
    c->inflight_packet_id = id;
    c->stats.publish_qos1++;
    c->last_activity_ms = Platform_GetTickMs();
    return MQTT_OK;
}

MqttStatus MqttClient_Subscribe(MqttClient *c, const char *topic, size_t topic_len,
                                uint8_t requested_qos)
{
    if (c == NULL)
        return MQTT_INVALID_ARG;
    if (c->state != MQTT_STATE_CONNECTED)
        return MQTT_NOT_CONNECTED;
    if (topic == NULL || topic_len == 0U || topic_len > MQTT_MAX_TOPIC_LENGTH ||
        requested_qos > 1U)
        return MQTT_INVALID_ARG;
    if (c->tx_pending || c->sub_pending)
        return MQTT_BUSY;

    uint16_t id = c->next_sub_id;
    c->next_sub_id = (c->next_sub_id == 0xFFFFU) ? 1U : (uint16_t)(c->next_sub_id + 1U);

    size_t n = MqttCodec_EncodeSubscribe(c->tx_buf, sizeof(c->tx_buf), topic, topic_len,
                                         requested_qos, id);
    if (n == 0U)
        return MQTT_PACKET_TOO_LARGE;
    StartTx(c, c->tx_buf, (uint32_t)n, TX_SUBSCRIBE);
    c->sub_pending = true;
    c->sub_packet_id = id;
    c->stats.subscribe_requests++;
    c->last_activity_ms = Platform_GetTickMs();
    return MQTT_OK;
}

MqttClientState MqttClient_GetState(const MqttClient *c)
{
    return c != NULL ? c->state : MQTT_STATE_ERROR;
}

void MqttClient_GetStats(const MqttClient *c, MqttStats *out)
{
    if (c == NULL || out == NULL) return;
    *out = c->stats;
}