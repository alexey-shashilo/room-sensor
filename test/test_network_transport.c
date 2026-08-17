#include <stdio.h>
#include <string.h>

#include "network_transport.h"
#include "fake_network_adapter.h"
#include "fake_platform_time.h"
#include "platform_time.h"

/* Phase 16 focused tests: state machine matrix, partial send/receive, remote
   close vs WOULD_BLOCK, endpoint validation, bounded connect timeout, and
   uint32 wrap. Deterministic; uses the virtual clock (no real sleep). */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static FakeNetworkAdapter        s_fake;
static NetworkTransportAdapter   s_adapter;
static NetworkTransport          s_t;
static NetworkEndpoint           s_ep;

static void reset_transport(void)
{
    FakePlatform_SetTick(0);
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    memset(&s_ep, 0, sizeof(s_ep));
    s_ep.port = 1883;
    memcpy(s_ep.host, "broker.example", 14);
    NetworkTransport_Init(&s_t, &s_adapter, &s_ep);
}

static void connect_ok(void)
{
    NetworkTransport_Connect(&s_t);
    FakePlatform_AdvanceTick(1);
    NetworkTransport_Run(&s_t);   /* immediate connect -> CONNECTED */
}

static NetworkStatus run_ms(uint32_t ms)
{
    NetworkStatus last = NET_OK;
    bool saw_timeout = false;
    bool saw_error = false;
    for (uint32_t i = 0; i < ms; i++)
    {
        FakePlatform_AdvanceTick(1);
        last = NetworkTransport_Run(&s_t);
        if (last == NET_TIMEOUT) saw_timeout = true;
        if (last == NET_TRANSPORT_ERROR) saw_error = true;
    }
    /* Prefer the most specific result: a timeout that fired must be reported even
       if later Runs (after entering ERROR) return TRANSPORT_ERROR. */
    if (saw_timeout) return NET_TIMEOUT;
    (void)saw_error;
    return last;
}

/* drain TX fully by running until no TX remains (send-all adapter). */
static void flush_tx(void)
{
    int guard = 0;
    while (s_t.tx_len > 0U && guard < 1000)
    {
        FakePlatform_AdvanceTick(1);
        NetworkTransport_Run(&s_t);
        guard++;
    }
}

/* ---- state machine matrix ---- */
static void test_state_machine(void)
{
    printf("\n== state machine matrix ==\n");

    reset_transport();
    T(NetworkTransport_GetState(&s_t) == NET_STATE_DISCONNECTED, "init state DISCONNECTED");
    T(NetworkTransport_Connect(&s_t) == NET_IN_PROGRESS, "Connect returns IN_PROGRESS");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTING, "state CONNECTING after Connect");
    T(NetworkTransport_Run(&s_t) == NET_OK, "Run completes connect -> CONNECTED");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTED, "state CONNECTED");

    /* CONNECTING -> timeout */
    reset_transport();
    s_fake.connect_mode = FAKE_NET_CONNECT_TIMEOUT;
    NetworkTransport_Connect(&s_t);
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTING, "timeout script: still CONNECTING");
    NetworkStatus ns = run_ms(10001U);
    T(ns == NET_TIMEOUT, "connect timeout after deadline");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_ERROR, "state ERROR after connect timeout");

    /* CONNECTING -> refused */
    reset_transport();
    s_fake.connect_mode = FAKE_NET_CONNECT_REFUSED;
    T(NetworkTransport_Connect(&s_t) == NET_TRANSPORT_ERROR, "Connect refusal -> TRANSPORT_ERROR");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_ERROR, "state ERROR after refusal");

    /* CONNECTED -> explicit disconnect; duplicate disconnect safe */
    reset_transport();
    connect_ok();
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "Disconnect returns OK");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_DISCONNECTED, "state DISCONNECTED after Disconnect");
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "duplicate Disconnect is safe");

    /* Connect while connected illegal */
    reset_transport();
    connect_ok();
    T(NetworkTransport_Connect(&s_t) == NET_ALREADY, "Connect while CONNECTED -> ALREADY");

    /* CONNECTED -> remote close */
    reset_transport();
    connect_ok();
    s_fake.recv_mode = FAKE_NET_RECV_REMOTE_CLOSE;
    NetworkStatus rc = NetworkTransport_Run(&s_t);
    T(rc == NET_REMOTE_CLOSED, "remote close detected by Run");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CLOSING, "state CLOSING after remote close");

    /* reconnect: CLOSING -> Disconnect -> Connect -> CONNECTED */
    reset_transport();
    connect_ok();
    s_fake.recv_mode = FAKE_NET_RECV_REMOTE_CLOSE;
    NetworkTransport_Run(&s_t);
    NetworkTransport_Disconnect(&s_t);
    s_fake.recv_mode = FAKE_NET_RECV_NONE;
    NetworkTransport_Connect(&s_t);
    NetworkTransport_Run(&s_t);
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTED, "reconnect after close reaches CONNECTED");
}

/* ---- partial send ---- */
static void test_partial_send(void)
{
    printf("\n== partial send ==\n");
    /* payload bigger than TX cap; adapter accepts no more than 16 bytes/call. */
    uint8_t payload[300];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i & 0xFF);

    reset_transport();
    connect_ok();
    s_fake.send_mode = FAKE_NET_SEND_CAPN;
    s_fake.send_cap  = 16U;

    /* Caller hands the whole 300 bytes; transport accepts up to TX cap (256) in
       one Send and returns the accepted count (partial). */
    size_t sent = 999;
    NetworkStatus s0 = NetworkTransport_Send(&s_t, payload, sizeof(payload), &sent);
    T(s0 == NET_OK, "Send returns OK");
    T(sent == NETWORK_TX_BUF_CAP, "Send accepts exactly TX_BUF_CAP (256) — partial");

    /* No bytes duplicated/skipped: TX ring contains the first 256 bytes in order. */
    int order_ok = 1;
    size_t idx = s_t.tx_tail;
    for (size_t c = 0; c < (size_t)s_t.tx_len; c++)
    {
        if (s_t.tx_buf[idx] != payload[c]) { order_ok = 0; break; }
        idx = (idx + 1) % NETWORK_TX_BUF_CAP;
    }
    T(order_ok, "TX ring preserves exact byte order of accepted prefix");

    /* Drain the accepted prefix to the adapter (partial 16-byte writes). */
    flush_tx();
    T(s_t.tx_len == 0U, "accepted prefix fully drained to adapter");
    T(s_fake.accepted_total == NETWORK_TX_BUF_CAP, "adapter received exactly the accepted prefix");

    /* Caller resumes from the exact offset (256). With TX now empty this fits. */
    size_t sent2 = 0;
    NetworkStatus r2 = NetworkTransport_Send(&s_t, payload + NETWORK_TX_BUF_CAP,
                                             sizeof(payload) - NETWORK_TX_BUF_CAP, &sent2);
    T(r2 == NET_OK && sent2 == sizeof(payload) - NETWORK_TX_BUF_CAP,
      "caller resumes from exact offset (remainder accepted)");

    /* Drain again; verify every byte reaches the wire exactly once. */
    flush_tx();
    T(s_t.tx_len == 0U, "TX fully drained after remainder");
    T(s_fake.accepted_total == sizeof(payload), "adapter received exactly full payload");
    T(s_t.tx_bytes == sizeof(payload), "transport tx_bytes == payload size");
}

/* ---- partial receive + remote close ---- */
static void test_partial_receive(void)
{
    printf("\n== partial receive / remote close ==\n");
    /* Arbitrary fragmented byte stream: [1,2,0,5,rest] */
    uint8_t frag[] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49};

    reset_transport();
    connect_ok();
    /* provide the fragments one call at a time with WOULD_BLOCK in between. */
    s_fake.recv_mode = FAKE_NET_RECV_FEED;

    /* Feed 1 byte, Run, read 1 byte. */
    FakeNetworkAdapter_FeedRecv(&s_fake, frag, 1);
    NetworkTransport_Run(&s_t);
    uint8_t b0 = 0; size_t g0 = 0;
    T(NetworkTransport_Receive(&s_t, &b0, 1, &g0) == NET_OK, "receive returns OK");
    T(g0 == 1U && b0 == 0x41, "received exactly 1 byte, correct value");

    /* Next: WOULD_BLOCK (no peer data). */
    s_fake.recv_mode = FAKE_NET_RECV_NONE;
    size_t g1 = 0; uint8_t b1 = 0;
    NetworkStatus st = NetworkTransport_Receive(&s_t, &b1, 1, &g1);
    T(st == NET_WOULD_BLOCK && g1 == 0U, "WOULD_BLOCK when no bytes (NOT remote close)");

    /* 2 bytes fed + Run. */
    FakeNetworkAdapter_FeedRecv(&s_fake, frag + 1, 2);
    NetworkTransport_Run(&s_t);
    uint8_t b2[2]; size_t g2 = 0;
    NetworkTransport_Receive(&s_t, b2, 2, &g2);
    T(g2 == 2U && b2[0] == 0x42 && b2[1] == 0x43, "next 2 bytes received in order");

    /* remaining 6 bytes fed + Run. */
    FakeNetworkAdapter_FeedRecv(&s_fake, frag + 3, 6);
    NetworkTransport_Run(&s_t);
    uint8_t b3[8]; size_t g3 = 0;
    NetworkTransport_Receive(&s_t, b3, 8, &g3);
    T(g3 == 6U, "remaining 6 bytes received");
    T(memcmp(b3, frag + 3, 6) == 0, "exact preservation of trailing bytes");

    /* Remote close: after EOF, Receive reports REMOTE_CLOSED with no bytes. */
    s_fake.recv_mode = FAKE_NET_RECV_REMOTE_CLOSE;
    NetworkTransport_Run(&s_t);   /* fetch sees EOF -> CLOSING */
    size_t g4 = 0;
    NetworkStatus rst = NetworkTransport_Receive(&s_t, b3, 8, &g4);
    T(g4 == 0U && rst == NET_REMOTE_CLOSED, "receive after EOF -> REMOTE_CLOSED distinct");

    /* WOULD_BLOCK != REMOTE_CLOSED: distinct statuses. */
    T(NET_WOULD_BLOCK != NET_REMOTE_CLOSED, "WOULD_BLOCK and REMOTE_CLOSED are distinct constants");
}

/* ---- endpoint validation ---- */
static void test_endpoint(void)
{
    printf("\n== endpoint validation ==\n");
    NetworkStatus r;

    /* NULL endpoint */
    r = NetworkTransport_Init(&s_t, &s_adapter, NULL);
    T(r == NET_INVALID_ARG, "NULL endpoint -> INVALID_ARG");

    /* port 0 */
    {
        NetworkEndpoint e; memset(&e, 0, sizeof(e)); e.port = 0; memcpy(e.host, "h", 1);
        r = NetworkTransport_Init(&s_t, &s_adapter, &e);
        T(r == NET_INVALID_ARG, "port 0 -> INVALID_ARG");
    }
    /* empty host */
    {
        NetworkEndpoint e; memset(&e, 0, sizeof(e)); e.port = 1;
        r = NetworkTransport_Init(&s_t, &s_adapter, &e);
        T(r == NET_INVALID_ARG, "empty host -> INVALID_ARG");
    }
    /* oversized host */
    {
        NetworkEndpoint e; memset(&e, 0, sizeof(e)); e.port = 1;
        memset(e.host, 'x', sizeof(e.host));   /* 65 bytes, no NUL within host[] */
        r = NetworkTransport_Init(&s_t, &s_adapter, &e);
        T(r == NET_INVALID_ARG, "oversized host -> INVALID_ARG");
    }
    /* max-valid host length (exactly 64 chars + NUL) is accepted. */
    {
        NetworkEndpoint e; memset(&e, 0, sizeof(e)); e.port = 1883;
        memset(e.host, 'a', 64); e.host[64] = '\0';
        r = NetworkTransport_Init(&s_t, &s_adapter, &e);
        T(r == NET_OK, "64-char host accepted");
    }
}

/* ---- timeout + uint32 wrap ---- */
static void test_timeout_wrap(void)
{
    printf("\n== timeout / uint32 wrap ==\n");

    /* Just-before deadline: connect not yet timed out. */
    reset_transport();
    s_fake.connect_mode = FAKE_NET_CONNECT_TIMEOUT;
    NetworkTransport_Connect(&s_t);
    NetworkStatus before = run_ms(9999U);
    (void)before;
    T(NetworkTransport_GetState(&s_t) != NET_STATE_ERROR, "no timeout just before deadline");

    /* At/after deadline: timeout fires. */
    reset_transport();
    s_fake.connect_mode = FAKE_NET_CONNECT_TIMEOUT;
    NetworkTransport_Connect(&s_t);
    NetworkStatus after = run_ms(10001U);
    T(after == NET_TIMEOUT, "timeout fires at/after deadline");

    /* UINT32 wrap: start virtual clock near 0xFFFFFF00, arm connect, run across wrap. */
    reset_transport();
    FakePlatform_SetTick(0xFFFFFF00U);
    s_fake.connect_mode = FAKE_NET_CONNECT_TIMEOUT;   /* poll stays in progress -> transport deadline */
    NetworkTransport_Connect(&s_t);
    /* Deadline ends at 0xFFFFFF00 + 10000 = 0x00002710 etc - crosses 0x00000000. */
    NetworkStatus wr = run_ms(10001U);
    T(wr == NET_TIMEOUT, "wrap-safe connect timeout fires across uint32 boundary");

    /* connect success across wrap (deadline does not spuriously fire). */
    reset_transport();
    FakePlatform_SetTick(0xFFFFFF00U);
    s_fake.connect_mode = FAKE_NET_CONNECT_DELAYED;
    s_fake.connect_delay_ms = 5000U;   /* succeeds at 0xFFFFFF00+5000 = 0xFFFFFF f4 -> within deadline */
    NetworkTransport_Connect(&s_t);
    NetworkStatus cs = run_ms(6000U);
    T(cs == NET_OK && NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTED,
      "delayed connect succeeds across wrap within deadline");
}

int main(void)
{
    printf("Phase 16 NetworkTransport focused tests\n");
    test_state_machine();
    test_partial_send();
    test_partial_receive();
    test_endpoint();
    test_timeout_wrap();
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}