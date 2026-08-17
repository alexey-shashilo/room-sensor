#ifndef NETWORK_TRANSPORT_H
#define NETWORK_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================
   Portable Network Transport (Phase 16).

   A cooperative, non-blocking, reliable byte-stream transport abstraction
   intended as the mechanism layer for a future MQTT-over-TCP client.

   Border discipline:
     - This layer is PURELY transport mechanism. It owns NO reconnect policy
       (that belongs to a future ConnectionManager / MQTT lifecycle layer).
     - It is byte-stream oriented; it does NOT parse MQTT, framing, topics, or
       QoS.
     - It performs NO network I/O itself: it drives a caller-provided adapter
       (function pointers) exactly like I2cBus drives adapter callbacks.
     - It uses ONLY the monotonic virtual clock (Platform_GetTickMs), never
       wall-clock sleeps.

   Future adapter boundary:
       NetworkTransport  (this file, portable; linked into portable core)
            |
            +-- host fake adapter (test/, host-only)
            |
            +-- future Wi-Fi/TCP adapter (later phase; NOT compiled into
                portable core until explicitly wired).

   NOTE: No physical adapter is implemented in this phase. The transport is
   exercised against a deterministic host fake. It is compiled into the portable
   core so a future adapter can be wired without touching this file.
   ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Explicit transport status / error model ------------------------------- */
typedef enum
{
    NET_OK = 0,          /* operation completed */
    NET_WOULD_BLOCK,     /* no bytes/not yet available; retry later (non-blocking).
                            NEVER equals remote-close/EOF. */
    NET_IN_PROGRESS,     /* a long operation (connect) is advancing; call again. */
    NET_INVALID_ARG,     /* NULL / empty / oversized argument. */
    NET_NOT_CONNECTED,   /* operation requires CONNECTED state. */
    NET_ALREADY,         /* operation illegal in current state (e.g. Connect while connected). */
    NET_REMOTE_CLOSED,   /* peer closed the connection (EOF), distinct from WOULD_BLOCK. */
    NET_TIMEOUT,         /* connection exceeded its bounded deadline. */
    NET_TRANSPORT_ERROR  /* adapter-level transport failure. */
} NetworkStatus;

typedef enum
{
    NET_STATE_DISCONNECTED = 0,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,
    NET_STATE_CLOSING,
    NET_STATE_ERROR
} NetworkState;

/* ---- Endpoint representation (bounded, no dynamic allocation) ------------- */
#define NETWORK_ENDPOINT_HOST_MAX  64U

typedef struct
{
    char     host[NETWORK_ENDPOINT_HOST_MAX + 1U];  /* +1 for NUL */
    uint16_t port;
} NetworkEndpoint;

/* ---- Adapter interface (transport mechanism calls into the adapter) ------- */
typedef struct NetworkTransportAdapter NetworkTransportAdapter;

struct NetworkTransportAdapter
{
    void *ctx;   /* adapter-private state */

    /* Start a non-blocking connect to `endpoint`. Returns NET_OK to accept the
       open (transport transitions to CONNECTING) or a terminal error. */
    NetworkStatus (*open)(void *ctx, const NetworkEndpoint *endpoint);

    /* Advance an in-flight connect. Returns NET_OK once CONNECTED, NET_IN_PROGRESS
       while still connecting, or a terminal error/timeout. */
    NetworkStatus (*poll)(void *ctx);

    /* Write up to `n` bytes from `data` to the wire. May accept fewer: writes
       exactly the accepted count into *sent and consumes only that many. Returns
       NET_OK (with *sent possibly 0), NET_WOULD_BLOCK (0 accepted), or terminal. */
    NetworkStatus (*send)(void *ctx, const uint8_t *data, size_t n, size_t *sent);

    /* Read up to `cap` bytes from the wire into `buf`. Sets *got to bytes read.
       Returns NET_WOULD_BLOCK when 0 bytes available (NOT EOF); returns
       NET_REMOTE_CLOSED on clean EOF. */
    NetworkStatus (*recv)(void *ctx, uint8_t *buf, size_t cap, size_t *got);

    /* Close the underlying socket/connection. */
    void (*close)(void *ctx);
};

/* ---- Bounded internal transfer budget -------------------------------------- */
#define NETWORK_TX_BUF_CAP   256U
#define NETWORK_RX_BUF_CAP   256U
#define NETWORK_MAX_RX_FETCH 64U   /* max bytes pulled from adapter per Run() */

typedef struct
{
    NetworkState state;

    const NetworkTransportAdapter *adapter;  /* bound at Init */
    NetworkEndpoint endpoint;                 /* bound at Connect */

    /* TX ring: bytes accepted from caller awaiting adapter send. */
    uint8_t  tx_buf[NETWORK_TX_BUF_CAP];
    size_t   tx_head, tx_tail;
    uint32_t tx_len;

    /* RX ring: bytes fetched from adapter awaiting caller Receive. */
    uint8_t  rx_buf[NETWORK_RX_BUF_CAP];
    size_t   rx_head, rx_tail;
    uint32_t rx_len;

    /* connect deadline (wrap-safe). */
    uint32_t connect_deadline_ms;
    bool     connect_timeout_set;

    /* stats (bounded).
     *   tx_accepted = bytes copied into the LOCAL TX ring by Send (local
     *                 acceptance; NOT wire delivery).
     *   tx_bytes    = bytes actually passed to the adapter successfully (wire
     *                 transmission). Under a partial/transient failure
     *                 tx_accepted can exceed tx_bytes; the LOCAL-accept counter
     *                 is intentionally DISTINCT from the wire-transmit counter. */
    uint32_t tx_accepted;          /* local acceptance */
    uint32_t tx_bytes;             /* adapter (wire) transmission */
    uint32_t rx_bytes;
    uint32_t connect_attempts, connect_failures;
    uint32_t remote_closed, transport_errors;
} NetworkTransport;

/* Stats snapshot for diagnostics/tests. */
typedef struct
{
    uint32_t tx_accepted;          /* local acceptance */
    uint32_t tx_bytes;             /* adapter (wire) transmission */
    uint32_t rx_bytes;
    uint32_t connect_attempts, connect_failures;
    uint32_t remote_closed, transport_errors;
} NetworkStats;

/* ---- Public API ------------------------------------------------------------ */

/* Bind `t` to `adapter` and a default endpoint. Returns NET_INVALID_ARG on
 * NULL/empty/oversized inputs. Does NOT open the connection. */
NetworkStatus NetworkTransport_Init(NetworkTransport *t,
                                    const NetworkTransportAdapter *adapter,
                                    const NetworkEndpoint *endpoint);

/* Enter CONNECTING and arm the bounded connect deadline (default 10 s). */
NetworkStatus NetworkTransport_Connect(NetworkTransport *t);

/* Explicit disconnect: stop the connection, free internal buffers, enter
 * DISCONNECTED. Safe to call from any state except an in-progress Run. */
NetworkStatus NetworkTransport_Disconnect(NetworkTransport *t);

/* Cooperative progress. Advance connect, drain TX toward the adapter, fetch
 * incoming bytes into RX. Must be called regularly from a watchdog-safe loop.
 * Returns NET_IN_PROGRESS while connect is in flight, NET_REMOTE_CLOSED once a
 * remote close is detected, or NET_OK otherwise. */
NetworkStatus NetworkTransport_Run(NetworkTransport *t);

/* Enqueue up to `n` bytes for sending.
 *
 * SEMANTICS OF `accepted` (Phase 16.1 adjudication):
 *   `*accepted` = the number of bytes COPIED INTO the transport's LOCAL TX ring
 *   in THIS call. It is NONE of the following:
 *     - bytes transmitted on the network,
 *     - bytes acknowledged by the peer,
 *     - bytes delivered to any broker/peer layer.
 *   It is purely LOCAL ACCEPTANCE into a bounded queue. The caller MUST resume
 *   from `*accepted` (`data + *accepted`, `n - *accepted`) once the accepted
 *   bytes have drained, because a full TX ring accepts fewer than `n`.
 *   Never blocks. */
NetworkStatus NetworkTransport_Send(NetworkTransport *t, const void *data,
                                    size_t n, size_t *accepted);

/* Dequeue up to `cap` already-received bytes. Returns NET_OK and writes the
 * number of bytes returned into *got (0 + NET_WOULD_BLOCK when nothing is
 * buffered and the stream is still open; NET_REMOTE_CLOSED when EOF + drained). */
NetworkStatus NetworkTransport_Receive(NetworkTransport *t, void *buf,
                                       size_t cap, size_t *got);

NetworkState  NetworkTransport_GetState(const NetworkTransport *t);
void          NetworkTransport_GetStats(const NetworkTransport *t, NetworkStats *out);

#ifdef __cplusplus
}
#endif

#endif