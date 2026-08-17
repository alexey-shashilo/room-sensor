#include <stdio.h>
#include <string.h>

#include "network_transport.h"
#include "fake_network_adapter.h"
#include "fake_platform_time.h"
#include "platform_time.h"

/* Phase 16.1 — byte-lifecycle contract adjudication.

   Freezes the exact semantics future MQTT will depend on:

   A. Send `accepted` == LOCAL acceptance, not wire delivery.
   B. Partial TX -> transport error -> reconnect: stale unsent TX is NOT replayed.
   C. Distinct counters: local-accepted (tx_accepted) != wire tx (tx_bytes).
   D. Buffered RX survives remote close (readable before EOF).
   E. No stale RX crosses a connection boundary.
   F. A late adapter "connected" after a connect timeout cannot resurrect the
      expired attempt.
   G. Disconnect is idempotent and cleans connection-scoped state.
   H. Zero-length Send/Receive are deterministic no-ops (not EOF).
   */

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
    NetworkTransport_Run(&s_t);
}

/* A. Send acceptance semantics */
static void test_send_acceptance(void)
{
    printf("\n== A. Send 'accepted' == local TX acceptance ==\n");
    reset_transport();
    connect_ok();

    size_t acc = 999;
    NetworkStatus s = NetworkTransport_Send(&s_t, "abcdefgh", 8, &acc);
    T(s == NET_OK && acc == 8U, "Send returns accepted=8 into local TX ring");
    /* accepted != peer-delivery: nothing transmitted yet (adapter untouched). */
    T(s_fake.accepted_total == 0U, "accepted is LOCAL only (no wire TX yet)");
    /* after a drain, local acceptance (8) matches wire transmission (8) on a
       fully-healthy link, but the two counters are distinct fields. */
    int d = 0;
    while (s_t.tx_len > 0U && d < 1000) { FakePlatform_AdvanceTick(1); NetworkTransport_Run(&s_t); d++; }
    NetworkStats st; NetworkTransport_GetStats(&s_t, &st);
    T(st.tx_accepted == 8U, "tx_accepted == 8 (local)");
    T(st.tx_bytes == 8U, "tx_bytes == 8 (wire) on healthy drain");
    T(st.tx_accepted == st.tx_bytes, "accepted==transmitted only when fully healthy");
}

/* B. Stale TX not replayed after reconnect */
static void test_stale_tx_not_replayed(void)
{
    printf("\n== B. stale TX not replayed across reconnect ==\n");
    reset_transport();
    connect_ok();

    /* Phase 1: transmit ABC on a healthy link (partially-written packet). */
    size_t acc = 0;
    NetworkTransport_Send(&s_t, "ABC", 3, &acc);
    T(acc == 3U, "queued ABC (accepted 3)");
    int d = 0;
    while (s_t.tx_len > 0U && d < 100) { FakePlatform_AdvanceTick(1); NetworkTransport_Run(&s_t); d++; }
    T(s_fake.accepted_total == 3U, "adapter transmitted ABC (3 bytes on wire)");

    /* Phase 2: queue DEFGH, then the adapter faults before sending it. */
    size_t acc2 = 0;
    NetworkTransport_Send(&s_t, "DEFGH", 5, &acc2);
    T(acc2 == 5U, "queued DEFGH (accepted 5, still local)");
    s_fake.send_error_pending = true;
    FakePlatform_AdvanceTick(1);
    NetworkTransport_Run(&s_t);   /* adapter errors -> terminal error: DEFGH discarded */
    T(s_t.tx_len == 0U, "terminal error discards unsent DEFGH (never buffered for retry)");
    T(s_fake.accepted_total == 3U, "wire still only 3 (DEFGH never transmitted)");

    /* reconnect: MUST be a clean connection (no stale DEFGH). */
    NetworkTransport_Disconnect(&s_t);
    T(s_t.tx_len == 0U, "Disconnect leaves TX empty");
    s_fake.send_mode = FAKE_NET_SEND_ALL;
    s_fake.recv_mode = FAKE_NET_RECV_NONE;
    connect_ok();

    /* new connection sends fresh JA; adapter lifetime total goes 3 -> 5. */
    size_t acc3 = 0;
    NetworkTransport_Send(&s_t, "JA", 2, &acc3);
    T(acc3 == 2U, "new connection enqueues fresh JA");
    d = 0;
    while (s_t.tx_len > 0U && d < 100) { FakePlatform_AdvanceTick(1); NetworkTransport_Run(&s_t); d++; }
    T(s_fake.accepted_total == 5U,
      "adapter on new conn received ONLY JA (stale DEFGH NOT replayed)");
    (void)d;
}

/* C. distinct counters under partial+error */
static void test_distinct_counters(void)
{
    printf("\n== C. accepted != wire-transmitted under partial+error ==\n");
    reset_transport();
    connect_ok();

    /* transmit ABC fully (wire=3). */
    size_t acc = 0;
    NetworkTransport_Send(&s_t, "ABC", 3, &acc);
    int d = 0;
    while (s_t.tx_len > 0U && d < 100) { FakePlatform_AdvanceTick(1); NetworkTransport_Run(&s_t); d++; }

    /* enqueue DEFGH (accepted 5, local), then error before wire. */
    size_t acc2 = 0;
    NetworkTransport_Send(&s_t, "DEFGH", 5, &acc2);
    s_fake.send_error_pending = true;
    FakePlatform_AdvanceTick(1);
    NetworkTransport_Run(&s_t);

    NetworkStats st; NetworkTransport_GetStats(&s_t, &st);
    T(st.tx_accepted == 8U, "tx_accepted == 8 (local: ABC+DEFGH)");
    T(st.tx_bytes == 3U, "tx_bytes == 3 (wire: only ABC)");
    T(st.tx_accepted != st.tx_bytes, "LOCAL_ACCEPTED_DISTINCT_FROM_ADAPTER_TX = YES");
    (void)d;
}

/* D. buffered RX survives remote close */
static void test_buffered_rx_survives_close(void)
{
    printf("\n== D. buffered RX survives remote close ==\n");
    reset_transport();
    uint8_t abc[3] = {'A','B','C'};
    connect_ok();
    /* Feed ABC (buffers it into RX), THEN peer closes. */
    FakeNetworkAdapter_FeedRecv(&s_fake, abc, 3);
    NetworkTransport_Run(&s_t);   /* buffers ABC into RX */
    T(s_t.rx_len == 3U, "ABC buffered into RX");

    /* Now the peer closes; on next Run, adapter reports EOF. */
    s_fake.recv_mode = FAKE_NET_RECV_REMOTE_CLOSE;
    NetworkTransport_Run(&s_t);
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CLOSING, "remote close -> CLOSING");

    /* Buffered ABC still readable. */
    uint8_t b[2]; size_t got = 0;
    NetworkStatus rs = NetworkTransport_Receive(&s_t, b, 2, &got);
    T(rs == NET_OK && got == 2U && b[0]=='A' && b[1]=='B', "buffered AB readable after EOF");
    uint8_t c1 = 0; size_t got2 = 0;
    NetworkStatus rs2 = NetworkTransport_Receive(&s_t, &c1, 1, &got2);
    T(rs2 == NET_OK && got2 == 1U && c1=='C', "buffered C readable after EOF");
    size_t got3 = 0;
    NetworkStatus rs3 = NetworkTransport_Receive(&s_t, b, 2, &got3);
    T(rs3 == NET_REMOTE_CLOSED && got3 == 0U, "next Receive -> REMOTE_CLOSED");
}

/* E. no stale RX across reconnect */
static void test_no_stale_rx_across_reconnect(void)
{
    printf("\n== E. no stale RX crosses connection boundary ==\n");
    reset_transport();
    connect_ok();
    uint8_t oldb[3] = {'O','L','D'};
    FakeNetworkAdapter_FeedRecv(&s_fake, oldb, 3);
    NetworkTransport_Run(&s_t);   /* OLD buffered into RX */
    T(s_t.rx_len == 3U, "OLD bytes buffered");

    /* disconnect + reconnect (new connection). */
    NetworkTransport_Disconnect(&s_t);
    T(s_t.rx_len == 0U, "Disconnect clears RX ring");
    connect_ok();

    /* feed NEW on connection B; caller must observe NEW only. */
    uint8_t newb[3] = {'N','E','W'};
    FakeNetworkAdapter_FeedRecv(&s_fake, newb, 3);
    NetworkTransport_Run(&s_t);
    uint8_t rb[4]; size_t got = 0;
    NetworkStatus rs = NetworkTransport_Receive(&s_t, rb, 4, &got);
    T(rs == NET_OK && got == 3U && memcmp(rb, "NEW", 3) == 0,
      "connection B yields NEW, never stale OLD");
}

/* F. late connect after timeout cannot resurrect */
static void test_late_connect_after_timeout(void)
{
    printf("\n== F. late adapter completion cannot resurrect timed-out attempt ==\n");
    reset_transport();
    s_fake.connect_mode = FAKE_NET_CONNECT_TIMEOUT;   /* poll stays IN_PROGRESS */
    NetworkTransport_Connect(&s_t);
    NetworkState st_after = NET_STATE_ERROR;
    int saw_timeout = 0;
    for (uint32_t i = 0; i < 10001U; i++)
    {
        FakePlatform_AdvanceTick(1);
        NetworkStatus r = NetworkTransport_Run(&s_t);
        if (r == NET_TIMEOUT) saw_timeout = 1;
        st_after = NetworkTransport_GetState(&s_t);
        if (st_after == NET_STATE_ERROR) break;
    }
    T(saw_timeout && st_after == NET_STATE_ERROR, "attempt timed out -> ERROR");

    /* Even if the fake later "completes", the transport never re-polls in ERROR,
       so the connection stays dead. */
    FakePlatform_AdvanceTick(1);
    NetworkTransport_Run(&s_t);   /* returns TRANSPORT_ERROR (state ERROR), no poll */
    T(NetworkTransport_GetState(&s_t) == NET_STATE_ERROR, "state stays ERROR (not resurrected)");

    /* A fresh explicit attempt after Disconnect works. */
    NetworkTransport_Disconnect(&s_t);
    s_fake.connect_mode = FAKE_NET_CONNECT_IMMEDIATE;
    NetworkTransport_Connect(&s_t);
    NetworkTransport_Run(&s_t);
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTED,
      "fresh explicit Connect creates a new successful attempt");
}

/* G. disconnect idempotency from all states */
static void test_disconnect_idempotency(void)
{
    printf("\n== G. disconnect idempotency / cleanup ==\n");
    /* from CONNECTED */
    reset_transport(); connect_ok();
    size_t tacc = 0;
    NetworkTransport_Send(&s_t, "x", 1, &tacc);
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "Disconnect from CONNECTED OK");
    T(s_t.tx_len == 0U && s_t.rx_len == 0U, "rings cleared after Disconnect(CONNECTED)");

    /* from CONNECTING */
    reset_transport();
    s_fake.connect_mode = FAKE_NET_CONNECT_DELAYED;
    s_fake.connect_delay_ms = 200U;
    NetworkTransport_Connect(&s_t);
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTING, "CONNECTING");
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "Disconnect from CONNECTING OK");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_DISCONNECTED, "-> DISCONNECTED");

    /* from ERROR */
    reset_transport();
    s_fake.connect_mode = FAKE_NET_CONNECT_REFUSED;
    NetworkTransport_Connect(&s_t);
    T(NetworkTransport_GetState(&s_t) == NET_STATE_ERROR, "ERROR");
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "Disconnect from ERROR OK");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_DISCONNECTED, "-> DISCONNECTED");

    /* from DISCONNECTED (double disconnect) */
    reset_transport();
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "Disconnect from DISCONNECTED OK (no-op)");
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "duplicate Disconnect still OK");
}

/* H. zero-length semantics */
static void test_zero_length(void)
{
    printf("\n== H. zero-length semantics ==\n");
    reset_transport();
    connect_ok();
    size_t acc=0, got=0;
    T(NetworkTransport_Send(&s_t, NULL, 0, &acc) == NET_OK && acc == 0U,
      "Send(0) -> NET_OK, accepted=0 (not EOF)");
    uint8_t b;
    NetworkStatus rs = NetworkTransport_Receive(&s_t, &b, 0, &got);
    T(rs == NET_WOULD_BLOCK || rs == NET_OK, "Receive(0) deterministic (no EOF)");
    T(got == 0U, "Receive(0) returns got=0");
}

/* Phase 16.2 — terminal transport-error state contract.
   A terminal adapter TRANSPORT_ERROR observed by Run() must move the transport
   out of CONNECTED (to ERROR) and discard connection-scoped TX. */
static void test_terminal_error_contract(void)
{
    printf("\n== I. terminal TRANSPORT_ERROR -> state ERROR ==\n");
    reset_transport();
    connect_ok();

    /* Queue ABCDEFGH (accepted 8). */
    size_t acc = 0;
    NetworkTransport_Send(&s_t, "ABCDEFGH", 8, &acc);
    T(acc == 8U, "queued ABCDEFGH (accepted 8)");

    /* Adapter wires exactly ABC (3) once, then faults. A single Run transmits ABC
       then observes the terminal TRANSPORT_ERROR. */
    s_fake.send_mode = FAKE_NET_SEND_CAPN_THEN_ERROR;
    s_fake.send_cap  = 3U;
    FakePlatform_AdvanceTick(1);
    NetworkStatus inner = NetworkTransport_Run(&s_t);
    (void)inner;

    /* After the terminal error is observed by Run: */
    NetworkState st = NetworkTransport_GetState(&s_t);
    T(st != NET_STATE_CONNECTED, "state is NOT CONNECTED after terminal TRANSPORT_ERROR");
    T(st == NET_STATE_ERROR, "state is ERROR after terminal TRANSPORT_ERROR");

    /* Unsent DEFGH must be discarded on entering ERROR (never re-eligible). */
    T(s_t.tx_len == 0U, "connection-scoped TX discarded on terminal error");
    T(s_fake.accepted_total == 3U, "adapter wired only ABC (DEFGH never attempted)");

    /* Repeated Run must NOT retry stale TX / replicate. */
    size_t wire_before = s_fake.accepted_total;
    int stuck = 0;
    for (int k = 0; k < 20; k++)
    {
        FakePlatform_AdvanceTick(1);
        NetworkStatus r = NetworkTransport_Run(&s_t);   /* state ERROR -> TRANSPORT_ERROR */
        if (r == NET_OK) stuck = 1;
    }
    T(!stuck, "Run from ERROR never reports OK (no retry)");
    T(s_fake.accepted_total == wire_before, "TX never retried after terminal error");

    /* Send while ERROR rejected. */
    size_t a2 = 999;
    NetworkStatus ss = NetworkTransport_Send(&s_t, "ZZ", 2, &a2);
    T(ss != NET_OK && a2 == 0U, "Send while ERROR rejected");

    /* Receive while ERROR deterministic (not NET_OK). */
    size_t g0 = 999;
    uint8_t b;
    NetworkStatus rs = NetworkTransport_Receive(&s_t, &b, 1, &g0);
    T(rs != NET_OK, "Receive while ERROR rejected (net E_NOT_CONNECTED/explicit)");
}

/* Disconnect from ERROR clears all connection state; a new Connect starts clean. */
static void test_error_disconnect_reconnect(void)
{
    printf("\n== J. ERROR -> Disconnect -> DISCONNECTED -> Connect -> CONNECTED ==\n");
    reset_transport();
    connect_ok();

    size_t acc = 0;
    NetworkTransport_Send(&s_t, "ABCDEFGH", 8, &acc);
    s_fake.send_mode = FAKE_NET_SEND_CAPN_THEN_ERROR;
    s_fake.send_cap  = 3U;
    FakePlatform_AdvanceTick(1);
    NetworkTransport_Run(&s_t);          /* terminal error -> ERROR */

    T(NetworkTransport_GetState(&s_t) == NET_STATE_ERROR, "pre: state ERROR");

    /* Disconnect from ERROR. */
    T(NetworkTransport_Disconnect(&s_t) == NET_OK, "Disconnect from ERROR OK");
    T(NetworkTransport_GetState(&s_t) == NET_STATE_DISCONNECTED, "-> DISCONNECTED");
    T(s_t.tx_len == 0U && s_t.rx_len == 0U, "Disconnect clears TX and RX");
    T(!s_t.connect_timeout_set, "Disconnect clears deadline latch");

    /* Fresh Connect to a healthy adapter succeeds; no old state leaks. */
    s_fake.send_mode = FAKE_NET_SEND_ALL;
    s_fake.recv_mode = FAKE_NET_RECV_NONE;
    T(NetworkTransport_Connect(&s_t) == NET_IN_PROGRESS, "new Connect starts a clean attempt");
    NetworkTransport_Run(&s_t);
    T(NetworkTransport_GetState(&s_t) == NET_STATE_CONNECTED, "reconnected CONNECTED");
    T(s_t.tx_len == 0U && s_t.rx_len == 0U, "new connection has empty rings");
}

/* Accounting: 8 locally accepted, 3 adapter-transmitted, rest discarded by error. */
static void test_error_accounting(void)
{
    printf("\n== K. accounting on terminal error ==\n");
    reset_transport();
    connect_ok();
    size_t acc = 0;
    NetworkTransport_Send(&s_t, "ABCDEFGH", 8, &acc);
    s_fake.send_mode = FAKE_NET_SEND_CAPN_THEN_ERROR;
    s_fake.send_cap  = 3U;
    FakePlatform_AdvanceTick(1);
    NetworkTransport_Run(&s_t);

    NetworkStats st; NetworkTransport_GetStats(&s_t, &st);
    T(st.tx_accepted == 8U, "tx_accepted == 8 (local)");
    T(st.tx_bytes == 3U, "tx_bytes == 3 (adapter)");
    T(st.tx_accepted != st.tx_bytes, "accepted != transmitted (discarded 5 not counted as wire)");
}

int main(void)
{
    printf("Phase 16.1 NetworkTransport byte-lifecycle contract tests\n");
    test_send_acceptance();
    test_stale_tx_not_replayed();
    test_distinct_counters();
    test_buffered_rx_survives_close();
    test_no_stale_rx_across_reconnect();
    test_late_connect_after_timeout();
    test_disconnect_idempotency();
    test_zero_length();
    test_terminal_error_contract();
    test_error_disconnect_reconnect();
    test_error_accounting();
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}