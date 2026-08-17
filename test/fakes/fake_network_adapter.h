#ifndef FAKE_NETWORK_ADAPTER_H
#define FAKE_NETWORK_ADAPTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "network_transport.h"

/* Deterministic, scriptable host fake for the NetworkTransportAdapter boundary
   (Phase 16). Reuses the Phase 15 virtual-clock pattern. No real sockets.

   Design: the fake owns two byte queues modelling the peer's view:
     - fake->peer_rx: bytes the PEER will deliver (transport recv pulls from here).
     - fake->peer_tx: bytes the PEER accepts (transport send writes into here, the
       "wire"), decoupled from the peer_rx stream so one-way scripts are simple.

   Scriptable outcomes / controls (deterministic, time-driven via the virtual
   clock):
     - connect: immediate success, delayed (N ms then success), refusal, timeout,
       or permanent failure.
     - send: accept-all, per-call cap (forced partial), WOULD_BLOCK, or error.
     - recv: exact next-fragment bytes, WOULD_BLOCK (no data), remote close, or a
       terminal transport error mid-stream.
     - fail_after / close_after are NOT used for reconnect (transport owns no
       policy); they only bound a single connection lifetime in tests.
   */

#define FAKE_NET_PEER_BUF  512U

typedef enum
{
    FAKE_NET_CONNECT_IMMEDIATE = 0,
    FAKE_NET_CONNECT_DELAYED,     /* success after `connect_delay_ms` run in (poll) */
    FAKE_NET_CONNECT_REFUSED,     /* open() returns error */
    FAKE_NET_CONNECT_TIMEOUT      /* poll() stays IN_PROGRESS past the transport deadline */
} FakeNetConnectMode;

typedef enum
{
    FAKE_NET_SEND_NONE = 0,       /* WOULD_BLOCK always */
    FAKE_NET_SEND_ALL,            /* accept everything */
    FAKE_NET_SEND_CAPN,           /* accept up to `send_cap` per call (partial) */
    FAKE_NET_SEND_OK16,           /* accept up to 16 bytes per call */
    FAKE_NET_SEND_ERROR,          /* transport error on first send */
    /* Accept up to `send_cap` bytes ONCE (a partially-written packet), then a
       terminal TRANSPORT_ERROR on every subsequent call. Deterministic model of
       "partial write then connection loss". */
    FAKE_NET_SEND_CAPN_THEN_ERROR
} FakeNetSendMode;

typedef enum
{
    FAKE_NET_RECV_NONE = 0,       /* WOULD_BLOCK always */
    FAKE_NET_RECV_FEED,           /* serve bytes from peer_buf */
    FAKE_NET_RECV_REMOTE_CLOSE,   /* peer closed: EOF */
    FAKE_NET_RECV_ERROR           /* transport error on next recv */
} FakeNetRecvMode;

typedef struct
{
    /* connection outcome. */
    FakeNetConnectMode connect_mode;
    uint32_t           connect_delay_ms;       /* ticks after ArmingConnect when poll succeeds */
    bool               connect_initiated;
    uint32_t           connect_start_tick;

    /* send behavior. */
    FakeNetSendMode    send_mode;
    size_t             send_cap;
    bool               send_error_pending;
    bool               send_cap_once_used;      /* CAPN_THEN_ERROR latch */

    /* recv behavior + peer bytes. */
    FakeNetRecvMode    recv_mode;
    uint8_t            peer_buf[FAKE_NET_PEER_BUF];
    size_t             peer_buf_len;           /* total bytes peer will send */
    size_t             peer_buf_pos;             /* next index to serve */
    bool               recv_error_pending;

    /* accounting. */
    size_t             accepted_total;           /* bytes peer accepted (sent to wire) */
    size_t             close_count;
    int                open_count;
    int                poll_count;
} FakeNetworkAdapter;

/* Bind a fake into a NetworkTransportAdapter (persistent in the caller). */
void FakeNetworkAdapter_GetAdapter(NetworkTransportAdapter *out, FakeNetworkAdapter *fake);

/* Queue `n` peer->device bytes (recv feed). */
void FakeNetworkAdapter_FeedRecv(FakeNetworkAdapter *fake, const uint8_t *data, size_t n);

/* Reset a fake to a deterministic default (disconnected, empty). */
void FakeNetworkAdapter_Reset(FakeNetworkAdapter *fake);

#endif