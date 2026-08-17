#include "network_transport.h"
#include "platform_time.h"
#include <string.h>

/* Portable, cooperative, non-blocking byte-stream network transport (Phase 16).

   This file is the transport MECHANISM only. It owns no reconnect policy, no
   MQTT framing, and no network I/O: all wire access is delegated to a
   caller-provided `NetworkTransportAdapter`. It relies solely on
   Platform_GetTickMs for time and never sleeps.

   All internal state lives in the caller-provided NetworkTransport struct, so
   this layer is re-entrant / multi-instance and uses no globals. No dynamic
   memory is used. All loops are bounded by the fixed TX/RX capacities.
   ================================================================ */

/* Wrap-safe deadline helper (established repository u32 semantics). */
static bool DeadlineElapsed(uint32_t now, uint32_t deadline, uint32_t rel_ms)
{
    uint32_t delta = (uint32_t)(now - deadline);
    return (delta < 0x80000000U) && (delta >= rel_ms);
}

static bool EndpointValid(const NetworkEndpoint *e)
{
    if (e == NULL) return false;
    if (e->port == 0U) return false;
    if (e->host[0] == '\0') return false;
    if (strlen(e->host) > NETWORK_ENDPOINT_HOST_MAX) return false;
    return true;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */

NetworkStatus NetworkTransport_Init(NetworkTransport *t,
                                    const NetworkTransportAdapter *adapter,
                                    const NetworkEndpoint *endpoint)
{
    if (t == NULL) return NET_INVALID_ARG;
    if (adapter == NULL || adapter->open == NULL || adapter->poll == NULL ||
        adapter->send == NULL || adapter->recv == NULL || adapter->close == NULL)
        return NET_INVALID_ARG;
    if (!EndpointValid(endpoint))
        return NET_INVALID_ARG;

    memset(t, 0, sizeof(*t));
    t->adapter = adapter;
    t->state = NET_STATE_DISCONNECTED;
    memcpy(&t->endpoint, endpoint, sizeof(*endpoint));
    return NET_OK;
}

NetworkStatus NetworkTransport_Connect(NetworkTransport *t)
{
    if (t == NULL) return NET_INVALID_ARG;
    if (t->state != NET_STATE_DISCONNECTED)
        return NET_ALREADY;

    NetworkStatus s = t->adapter->open(t->adapter->ctx, &t->endpoint);
    if (s != NET_OK)
    {
        t->state = NET_STATE_ERROR;
        t->connect_failures++;
        return s;
    }

    t->state = NET_STATE_CONNECTING;
    t->connect_attempts++;
    /* Bounded, wrap-safe connect deadline (10 s). */
    t->connect_deadline_ms = (uint32_t)(Platform_GetTickMs() + 10000U);
    t->connect_timeout_set = true;
    return NET_IN_PROGRESS;
}

NetworkStatus NetworkTransport_Disconnect(NetworkTransport *t)
{
    if (t == NULL) return NET_INVALID_ARG;

    if (t->adapter != NULL)
        t->adapter->close(t->adapter->ctx);

    t->state = NET_STATE_DISCONNECTED;
    t->tx_head = t->tx_tail = 0U;
    t->tx_len = 0U;
    t->rx_head = t->rx_tail = 0U;
    t->rx_len = 0U;
    t->connect_timeout_set = false;
    return NET_OK;
}

NetworkState NetworkTransport_GetState(const NetworkTransport *t)
{
    return t != NULL ? t->state : NET_STATE_DISCONNECTED;
}

void NetworkTransport_GetStats(const NetworkTransport *t, NetworkStats *out)
{
    if (t == NULL || out == NULL) return;
    out->tx_accepted = t->tx_accepted;
    out->tx_bytes = t->tx_bytes;
    out->rx_bytes = t->rx_bytes;
    out->connect_attempts = t->connect_attempts;
    out->connect_failures = t->connect_failures;
    out->remote_closed    = t->remote_closed;
    out->transport_errors = t->transport_errors;
}

/* Advance connect until settled (bounded, wrap-safe). */
static NetworkStatus RunConnect(NetworkTransport *t)
{
    if (!t->connect_timeout_set)
        return NET_IN_PROGRESS;

    NetworkStatus ps = t->adapter->poll(t->adapter->ctx);
    if (ps == NET_OK)
    {
        t->state = NET_STATE_CONNECTED;
        t->connect_timeout_set = false;
        return NET_OK;
    }
    if (ps == NET_IN_PROGRESS)
    {
        if (DeadlineElapsed(Platform_GetTickMs(), t->connect_deadline_ms, 0U))
        {
            t->state = NET_STATE_ERROR;
            t->connect_timeout_set = false;
            t->connect_failures++;
            return NET_TIMEOUT;
        }
        return NET_IN_PROGRESS;
    }

    t->state = NET_STATE_ERROR;
    t->connect_timeout_set = false;
    t->connect_failures++;
    if (ps == NET_TRANSPORT_ERROR) t->transport_errors++;
    return ps;
}

/* Drain buffered TX into the adapter, honoring partial sends (bounded). */
static NetworkStatus DrainTx(NetworkTransport *t)
{
    while (t->tx_len > 0U)
    {
        /* Gather the contiguous pending TX slice. */
        uint8_t chunk[NETWORK_TX_BUF_CAP];
        size_t  chunk_len = 0U;
        size_t  i = t->tx_tail;
        while (chunk_len < (size_t)t->tx_len && chunk_len < NETWORK_TX_BUF_CAP)
        {
            chunk[chunk_len++] = t->tx_buf[i];
            i = (i + 1U) % NETWORK_TX_BUF_CAP;
        }

        size_t sent = 0U;
        NetworkStatus rs = t->adapter->send(t->adapter->ctx, chunk, chunk_len, &sent);
        if (rs == NET_WOULD_BLOCK)
            return NET_WOULD_BLOCK;
        if (rs != NET_OK)
        {
            if (rs == NET_TRANSPORT_ERROR) t->transport_errors++;
            else if (rs == NET_REMOTE_CLOSED) t->remote_closed++;
            return rs;
        }
        if (sent == 0U)
            return NET_WOULD_BLOCK;

        uint32_t actual = (uint32_t)sent;
        if (actual > t->tx_len) actual = t->tx_len;
        for (uint32_t k = 0U; k < actual; k++)
        {
            t->tx_buf[t->tx_tail] = 0U;
            t->tx_tail = (t->tx_tail + 1U) % NETWORK_TX_BUF_CAP;
        }
        t->tx_len -= actual;
        t->tx_bytes += actual;
    }
    return NET_OK;
}

/* Fetch incoming bytes from the adapter into RX, bounded per call. */
static NetworkStatus RunRx(NetworkTransport *t)
{
    uint32_t free_rx = (uint32_t)NETWORK_RX_BUF_CAP - t->rx_len;
    if (free_rx == 0U)
        return NET_WOULD_BLOCK;

    uint32_t want = free_rx > NETWORK_MAX_RX_FETCH ? NETWORK_MAX_RX_FETCH : free_rx;
    uint8_t  tmp[NETWORK_MAX_RX_FETCH];
    size_t   got = 0U;
    NetworkStatus rs = t->adapter->recv(t->adapter->ctx, tmp, want, &got);

    if (rs == NET_REMOTE_CLOSED)
        return NET_REMOTE_CLOSED;   /* caller transitions to CLOSING */
    if (rs == NET_WOULD_BLOCK || got == 0U)
        return NET_WOULD_BLOCK;
    if (rs != NET_OK)
    {
        if (rs == NET_TRANSPORT_ERROR) t->transport_errors++;
        return rs;
    }

    for (uint32_t k = 0U; k < (uint32_t)got; k++)
    {
        t->rx_buf[t->rx_head] = tmp[k];
        t->rx_head = (t->rx_head + 1U) % NETWORK_RX_BUF_CAP;
    }
    t->rx_len += (uint32_t)got;
    t->rx_bytes += (uint32_t)got;
    return NET_OK;
}

/* ---------------------------------------------------------------- */
/* Terminal-error transition.

   A terminal adapter error (TRANSPORT_ERROR / REMOTE_CLOSED while sending, or a
   transport error while receiving) means the current connection can no longer be
   trusted. The transport:
     - transitions to ERROR (never stays CONNECTED),
     - discards connection-scoped TX bytes (the transport cannot know how much of
       a partially-written higher-level packet reached the peer), and
     - discards connection-scoped RX bytes (unlike orderly EOF, the stream is not
       guaranteed complete).

   Reconnect ownership is NOT here: the transport only renders ERROR; a policy
   layer decides when/how to reconnect via an explicit Disconnect + Connect. */
static void EnterError(NetworkTransport *t)
{
    t->state = NET_STATE_ERROR;
    /* Drop connection-scoped TX. */
    t->tx_head = t->tx_tail = 0U;
    t->tx_len = 0U;
    /* Drop connection-scoped RX. */
    t->rx_head = t->rx_tail = 0U;
    t->rx_len = 0U;
    t->connect_timeout_set = false;
}

/* Cooperative, non-blocking progress. */
NetworkStatus NetworkTransport_Run(NetworkTransport *t)
{
    if (t == NULL) return NET_INVALID_ARG;
    if (t->state == NET_STATE_DISCONNECTED)
        return NET_NOT_CONNECTED;
    if (t->state == NET_STATE_ERROR)
        return NET_TRANSPORT_ERROR;

    if (t->state == NET_STATE_CONNECTING)
        return RunConnect(t);

    if (t->state == NET_STATE_CONNECTED)
    {
        NetworkStatus s = DrainTx(t);
        if (s != NET_OK && s != NET_WOULD_BLOCK)
        {
            /* Terminal adapter failure during a send: the connection and any
               unsent bytes are no longer trustworthy. */
            EnterError(t);
            return s;
        }

        NetworkStatus r = RunRx(t);
        if (r == NET_REMOTE_CLOSED)
        {
            /* Orderly EOF: keep buffered RX readable; arrive at CLOSING. */
            t->state = NET_STATE_CLOSING;
            t->remote_closed++;
            return NET_REMOTE_CLOSED;
        }
        if (r != NET_OK && r != NET_WOULD_BLOCK)
        {
            /* Terminal adapter failure during a receive: discard the (potentially
               incomplete) buffered RX and enter ERROR. */
            EnterError(t);
            return r;
        }
    }
    return NET_OK;
}

/* ---------------------------------------------------------------- */
/* Send / Receive                                                      */

NetworkStatus NetworkTransport_Send(NetworkTransport *t, const void *data,
                                    size_t n, size_t *accepted)
{
    if (t == NULL || accepted == NULL)
        return NET_INVALID_ARG;
    *accepted = 0U;   /* default: nothing accepted on any rejection path */
    if (data == NULL && n != 0U)
        return NET_INVALID_ARG;
    if (t->state != NET_STATE_CONNECTED)
        return NET_NOT_CONNECTED;
    if (n == 0U)
        return NET_OK;   /* zero-byte send: no-op, *accepted == 0 */

    const uint8_t *src = (const uint8_t *)data;
    while (n > 0U && t->tx_len < (uint32_t)NETWORK_TX_BUF_CAP)
    {
        t->tx_buf[t->tx_head] = src[*accepted];
        t->tx_head = (t->tx_head + 1U) % NETWORK_TX_BUF_CAP;
        t->tx_len++;
        t->tx_accepted++;        /* local acceptance, distinct from wire tx */
        (*accepted)++;
        n--;
    }
    return NET_OK;
}

NetworkStatus NetworkTransport_Receive(NetworkTransport *t, void *buf,
                                       size_t cap, size_t *got)
{
    if (t == NULL || got == NULL)
        return NET_INVALID_ARG;
    if (t->state != NET_STATE_CONNECTED && t->state != NET_STATE_CLOSING)
        return NET_NOT_CONNECTED;
    if (buf == NULL && cap != 0U)
        return NET_INVALID_ARG;
    *got = 0U;

    if (t->rx_len > 0U)
    {
        size_t n = (size_t)t->rx_len;
        size_t room = cap;
        uint8_t *dst = (uint8_t *)buf;
        if (dst == NULL) room = 0U;
        if (n > room) n = room;
        for (size_t k = 0U; k < n; k++)
        {
            if (dst != NULL) dst[k] = t->rx_buf[t->rx_tail];
            t->rx_tail = (t->rx_tail + 1U) % NETWORK_RX_BUF_CAP;
            t->rx_len--;
        }
        *got = n;
        /* Bytes were delivered this call: report NET_OK. Remote close is only
           surfaced once the RX is fully drained AND no bytes are returned now
           (so a reader never mistakes delivered bytes for "already closed"). */
        if (n > 0U)
            return NET_OK;
        /* n == 0 (e.g. zero-length `cap`): fall through to EOF/WOULD_BLOCK. */
    }

    if (t->state == NET_STATE_CLOSING)
        return NET_REMOTE_CLOSED;
    return NET_WOULD_BLOCK;
}