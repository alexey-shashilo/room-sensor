#include "fake_network_adapter.h"
#include "fake_platform_time.h"
#include <string.h>

static void net_close(void *ctx)
{
    FakeNetworkAdapter *f = (FakeNetworkAdapter *)ctx;
    if (f == NULL) return;
    f->close_count++;
    f->connect_initiated = false;
}

static NetworkStatus net_open(void *ctx, const NetworkEndpoint *ep)
{
    FakeNetworkAdapter *f = (FakeNetworkAdapter *)ctx;
    if (f == NULL || ep == NULL) return NET_INVALID_ARG;

    f->open_count++;
    f->connect_initiated = true;
    f->connect_start_tick = FakePlatform_GetTick();

    switch (f->connect_mode)
    {
        case FAKE_NET_CONNECT_REFUSED:
            return NET_TRANSPORT_ERROR;
        case FAKE_NET_CONNECT_DELAYED:
        case FAKE_NET_CONNECT_TIMEOUT:
        case FAKE_NET_CONNECT_IMMEDIATE:
        default:
            return NET_OK;
    }
}

static NetworkStatus net_poll(void *ctx)
{
    FakeNetworkAdapter *f = (FakeNetworkAdapter *)ctx;
    if (f == NULL) return NET_INVALID_ARG;
    f->poll_count++;

    if (!f->connect_initiated)
        return NET_TRANSPORT_ERROR;

    switch (f->connect_mode)
    {
        case FAKE_NET_CONNECT_IMMEDIATE:
            return NET_OK;
        case FAKE_NET_CONNECT_DELAYED:
            if ((uint32_t)(FakePlatform_GetTick() - f->connect_start_tick) >= f->connect_delay_ms)
                return NET_OK;
            return NET_IN_PROGRESS;
        case FAKE_NET_CONNECT_TIMEOUT:
            return NET_IN_PROGRESS;   /* transport deadline will fire */
        case FAKE_NET_CONNECT_REFUSED:
        default:
            return NET_TRANSPORT_ERROR;
    }
}

static NetworkStatus net_send(void *ctx, const uint8_t *data, size_t n, size_t *sent)
{
    (void)data;
    FakeNetworkAdapter *f = (FakeNetworkAdapter *)ctx;
    if (f == NULL || sent == NULL) return NET_INVALID_ARG;
    *sent = 0U;

    if (f->send_error_pending)
    {
        f->send_error_pending = false;
        return NET_TRANSPORT_ERROR;
    }

    switch (f->send_mode)
    {
        case FAKE_NET_SEND_NONE:
            return NET_WOULD_BLOCK;
        case FAKE_NET_SEND_ERROR:
            return NET_TRANSPORT_ERROR;
        case FAKE_NET_SEND_CAPN:
        case FAKE_NET_SEND_OK16:
        {
            size_t cap = (f->send_mode == FAKE_NET_SEND_CAPN) ? f->send_cap : 16U;
            size_t take = n > cap ? cap : n;
            *sent = take;
            f->accepted_total += take;
            return NET_OK;
        }
        case FAKE_NET_SEND_CAPN_THEN_ERROR:
        {
            if (f->send_cap_once_used)
                return NET_TRANSPORT_ERROR;
            f->send_cap_once_used = true;
            size_t take = n > f->send_cap ? f->send_cap : n;
            *sent = take;                      /* partial write */
            f->accepted_total += take;
            return NET_OK;
        }
        case FAKE_NET_SEND_ALL:
        default:
            *sent = n;
            f->accepted_total += n;
            return NET_OK;
    }
}

static NetworkStatus net_recv(void *ctx, uint8_t *buf, size_t cap, size_t *got)
{
    FakeNetworkAdapter *f = (FakeNetworkAdapter *)ctx;
    if (f == NULL || got == NULL) return NET_INVALID_ARG;
    *got = 0U;

    if (f->recv_error_pending)
    {
        f->recv_error_pending = false;
        return NET_TRANSPORT_ERROR;
    }

    switch (f->recv_mode)
    {
        case FAKE_NET_RECV_NONE:
            return NET_WOULD_BLOCK;
        case FAKE_NET_RECV_REMOTE_CLOSE:
            if (f->peer_buf_pos >= f->peer_buf_len)
                return NET_REMOTE_CLOSED;
            /* serve remaining, then EOF on next call */
            return NET_REMOTE_CLOSED;   /* simplest: already closed; no bytes served */
        case FAKE_NET_RECV_ERROR:
            return NET_TRANSPORT_ERROR;
        case FAKE_NET_RECV_FEED:
        default:
        {
            size_t avail = f->peer_buf_len - f->peer_buf_pos;
            if (avail == 0U)
                return NET_WOULD_BLOCK;
            size_t take = avail > cap ? cap : avail;
            if (buf != NULL && take > 0U)
            {
                for (size_t i = 0; i < take; i++)
                    buf[i] = f->peer_buf[f->peer_buf_pos + i];
            }
            f->peer_buf_pos += take;
            *got = take;
            return NET_OK;
        }
    }
}

void FakeNetworkAdapter_Reset(FakeNetworkAdapter *f)
{
    if (f == NULL) return;
    memset(f, 0, sizeof(*f));
    f->connect_mode = FAKE_NET_CONNECT_IMMEDIATE;
    f->send_mode = FAKE_NET_SEND_ALL;
    f->recv_mode = FAKE_NET_RECV_NONE;
    f->send_cap = 0U;
}

void FakeNetworkAdapter_GetAdapter(NetworkTransportAdapter *out, FakeNetworkAdapter *fake)
{
    if (out == NULL || fake == NULL) return;
    out->ctx = fake;
    out->open = net_open;
    out->poll = net_poll;
    out->send = net_send;
    out->recv = net_recv;
    out->close = net_close;
}

void FakeNetworkAdapter_FeedRecv(FakeNetworkAdapter *f, const uint8_t *data, size_t n)
{
    if (f == NULL) return;
    if (data == NULL || n == 0U) { f->recv_mode = FAKE_NET_RECV_NONE; return; }
    if (n > FAKE_NET_PEER_BUF) n = FAKE_NET_PEER_BUF;
    memcpy(f->peer_buf, data, n);
    f->peer_buf_len = n;
    f->peer_buf_pos = 0U;
    f->recv_mode = FAKE_NET_RECV_FEED;
}