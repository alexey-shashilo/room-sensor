# Network Transport (Phase 16)

A portable, cooperative, non-blocking, reliable **byte-stream** transport
abstraction intended as the *mechanism* layer for a future MQTT-over-TCP client.
It is deliberately free of any application protocol, physical adapter, TLS, or
reconnect policy.

## Table of contents

1. Responsibilities
2. What it does NOT own
3. State machine
4. Error model
5. Endpoint representation
6. Buffer ownership / memory safety
7. Partial send semantics
8. Partial receive semantics
9. Timeout semantics
10. Reconnect ownership
11. Adapter boundary / future mapping
12. Non-blocking guarantees
13. Build isolation

---

## 1. Responsibilities

- Represent a single reliable byte-stream connection (`DISCONNECTED -> CONNECTING
  -> CONNECTED -> CLOSING/ERROR`).
- Buffer outbound bytes in a bounded TX ring and drain them across `Run()`
  calls, honoring adapter partial sends.
- Buffer inbound bytes in a bounded RX ring and hand them to the caller on
  `Receive()`, preserving exact order and counting.
- Distinguish `WOULD_BLOCK` from `REMOTE_CLOSED`.
- Enforce a bounded, wrap-safe connect timeout.
- Track bounded diagnostic counters.

## 2. What it does NOT own

- **No reconnect policy.** The transport never re-connects on its own. A future
  ConnectionManager / MQTT lifecycle layer owns connect/backoff/reconnect
  decisions. The test harness contains a *test-only* reconnect driver to prove
  the mechanism; it is never in production transport code.
- **No MQTT.** No packet codec, CONNECT/PUBLISH/SUBSCRIBE, topics, QoS, or broker
  behavior.
- **No physical network I/O.** The transport delegates all wire access to a
  caller-provided adapter. There is no Wi-Fi/TCP adapter in the portable core.
- **No threads / RTOS / malloc.** It is single-shot cooperative code using only
  the provided `NetworkTransport` struct and `Platform_GetTickMs`.

## 3. State machine

```
                 Connect()
DISCONNECTED ----------------> CONNECTING
     ^                            |   (adapter poll)
     |                            v
     |         (timeout / error)  |   success
  Disconnect()                CONNECTED
     ^                            |   remote close (Run/Rx)
     |                            v
     +-------- Disconnect() <--- CLOSING
                / ERROR
                 (explicitly reset to DISCONNECTED before a reconnect attempt)
```

Transitions:

| From          | Event             | To          | Result           |
|---------------|-------------------|-------------|------------------|
| DISCONNECTED  | `Connect()`       | CONNECTING  | `NET_IN_PROGRESS`|
| CONNECTING    | `Run()` poll OK   | CONNECTED   | `NET_OK`         |
| CONNECTING    | `Run()` deadline  | ERROR       | `NET_TIMEOUT`    |
| CONNECTING    | `Run()` poll err  | ERROR       | error status     |
| DISCONNECTED  | `Connect()` refus.| ERROR       | error status     |
| CONNECTED     | `Run()` EOF       | CLOSING     | `NET_REMOTE_CLOSED` |
| CONNECTED     | `Disconnect()`    | DISCONNECTED| `NET_OK`         |
| CONNECTED     | `Connect()`       | (unchanged) | `NET_ALREADY`    |
| ERROR         | `Disconnect()`    | DISCONNECTED| `NET_OK` (reset) |

## 4. Error model

A small explicit status set:

| Status           | Meaning                                        |
|------------------|------------------------------------------------|
| `NET_OK`         | operation completed                            |
| `NET_WOULD_BLOCK`| no data / not ready; retry later (NOT EOF)      |
| `NET_IN_PROGRESS`| long op (connect) advancing; call again        |
| `NET_INVALID_ARG`| NULL / empty / oversized argument              |
| `NET_NOT_CONNECTED`| op requires CONNECTED                        |
| `NET_ALREADY`    | illegal in current state (e.g. Connect while connected) |
| `NET_REMOTE_CLOSED`| peer closed (EOF)                            |
| `NET_TIMEOUT`    | bounded deadline exceeded                       |
| `NET_TRANSPORT_ERROR` | adapter-level transport failure           |

`NET_WOULD_BLOCK` and `NET_REMOTE_CLOSED` are explicitly distinct, so future
MQTT framing can tell "not yet" from "connection gone".

## 5. Endpoint representation

Bounded, fixed, no dynamic allocation:

```c
typedef struct { char host[65]; uint16_t port; } NetworkEndpoint;
```

- `host` NUL-terminated, max 64 chars + NUL.
- Validation rejects NULL, empty host, port 0, and host longer than 64.
- DNS resolution is left to the adapter; `NetworkEndpoint` is only a descriptor.

## 6. Buffer ownership / memory safety

- **Send:** caller owns `data` for the duration of the call. The transport copies
  accepted bytes into its bounded TX ring; it never retains the caller pointer.
- **Receive:** caller provides the destination buffer; transport copies `got`
  bytes out.
- Two bounded internal buffers (256 B each). **No dynamic allocation, no
  unbounded queue.**
- All loops perform a bounded amount of work per call.

### Byte-lifecycle contract (Phase 16.1)

The transport distinguishes, by design:

- **local acceptance** (`tx_accepted`): bytes copied into the local TX ring by
  `Send`. This is NOT wire delivery, NOT peer acknowledgement, NOT broker
  delivery.
- **wire transmission** (`tx_bytes`): bytes actually handed to the adapter
  successfully.
- Under a partial/transient failure `tx_accepted` may exceed `tx_bytes`. No
  counter labels local acceptance as wire transmission.

A higher protocol layer (e.g. future MQTT) treats local acceptance as the only
"delivered to the transport" signal and owns retransmission/replay decisions
after a connection loss.

## 7. Partial send semantics

`NetworkTransport_Send(t, data, n, &accepted)`:

- Returns `NET_OK` and sets `*accepted` to the number of bytes **accepted into the
  TX ring** (≤ `n`, ≤ TX cap). This is LOCAL ACCEPTANCE only.
- The caller MUST resume from `*accepted` (`data + *accepted`, `n - *accepted`)
  after the accepted bytes drain.
- A full TX ring accepts 0 bytes (`*accepted == 0`, still `NET_OK`) — the caller
  retries later.
- Zero-length send is a no-op (`NET_OK`, `*accepted == 0`); it is NOT EOF.
- `Run()` then drains TX to the adapter in bounded chunks, committing exactly
  what the adapter accepts (`tx_bytes`). No bytes are duplicated or dropped.

### Terminal connection loss & pending TX

A terminal failure means the current connection can no longer be trusted. The
transport distinguishes two cases:

- **Terminal `TRANSPORT_ERROR`** (send or receive adapter failure): the transport
  immediately transitions to `ERROR` and **discards all connection-scoped TX and
  RX bytes**. It can be sent nothing further, and never returns to `CONNECTED`
  without an explicit `Disconnect` + `Connect`. The transport cannot know how much
  of a partially-written higher-level packet reached the peer, so it will not
  retry or replay any of it.
- **Orderly `REMOTE_CLOSED`** (EOF): buffered RX is preserved for the caller to
  drain, and the transport reaches `CLOSING`. Any unsent TX is not replayed
  either; a reconnect starts fresh.

A subsequent `Connect` (after an explicit `Disconnect`) starts a fresh connection
with empty TX and RX rings. **No stale TX or RX bytes are replayed onto a new
connection.** The higher protocol layer owns retransmission decisions.

### Terminal-error state & RX policy (Phase 16.2)

- After `Run()` observes a terminal `TRANSPORT_ERROR`, `GetState()` **MUST NOT
  report `CONNECTED`**; it reports `ERROR`.
- `TRANSPORT_ERROR_RX_POLICY = DISCARD`: on a transport error the buffered RX is
  discarded (unlike orderly EOF, the received byte stream is not guaranteed
  complete).
- `Send()` and `Receive()` while `ERROR` are rejected (return a non-`NET_OK`
  status, and `Send` reports nothing accepted).
- Reconnect is NOT automatic: `NETWORK_TRANSPORT_OWNS_RECONNECT_POLICY = NO`.

## 8. Partial receive semantics

- Adapter `recv` fills the RX ring (bounded per `Run()`).
- `Receive(t, buf, cap, &got)` returns `*got` bytes, exact order, `NET_OK`.
- Empty RX ring + open stream => `NET_WOULD_BLOCK`, `*got == 0`.
- Peer EOF => transport enters `CLOSING`; once RX is drained, `Receive` returns
  `NET_REMOTE_CLOSED`.

The transport is a **byte stream**: it creates no message boundaries and does not
parse framing.

### Buffered RX survives remote close

When EOF is observed (`adapter recv -> REMOTE_CLOSED`), any bytes already fetched
into the RX ring remain readable. `Receive` first returns the buffered bytes
(`NET_OK`), and only after `rx_len` reaches zero does it report
`NET_REMOTE_CLOSED`. Already-received data is never discarded merely because EOF
was seen.

### No stale RX across a connection boundary

`Disconnect` clears the RX ring. A fresh `Connect` starts with an empty RX ring,
so unread bytes from a previous connection can never be observed as data from the
new connection.

## 9. Timeout semantics

- Connect arms a deadline: `deadline = tick + 10000`.
- `Run()` compares via a wrap-safe elapsed check (the repository's established
  `uint32_t` semantics — no naive `now >= deadline` that breaks at 2^32).
- Bound: a connect either completes, or renders `NET_TIMEOUT` after the deadline.

### Late adapter completion after timeout (adjudication)

Once a connect attempt times out, the transport leaves `CONNECTING` and enters
`ERROR`. It never re-queries the old adapter `poll` again, so a "late success"
from the expired attempt **cannot resurrect** the timed-out connection. Only an
explicit, fresh `Connect()` (from `DISCONNECTED`, reached via `Disconnect`)
starts a new attempt.

### Disconnect & connection-boundary cleanup

`Disconnect` is safe and idempotent from `CONNECTED`, `CONNECTING`, `ERROR`, and
`DISCONNECTED`. Every terminal cleanup resets the TX ring (discarding pending
connection-scoped outbound bytes) and the RX ring (discarding unread inbound
bytes), plus the connect deadline latch. No per-connection state leaks into the
next connection. Lifetime diagnostic counters (`tx_accepted`, `tx_bytes`, ...)
are not reset by `Disconnect`, so they are NOT a source of stale bytes for a new
connection.

### Zero-length semantics

- `Send(buf, 0)` -> `NET_OK`, `*accepted == 0`. A no-op; it does **not** mean
  EOF.
- `Receive(buf, 0)` -> `NET_OK`, `*got == 0` when buffered bytes remain (caller
  chose to read none); `NET_WOULD_BLOCK` when empty + open; `NET_REMOTE_CLOSED`
  when CLOSING + drained. Never treated as EOF by itself.

## 10. Reconnect ownership

**`NETWORK_TRANSPORT_OWNS_RECONNECT_POLICY = NO`.**

The mechanism stops at a terminal state; a future policy layer decides when/how to
reconnect. Test-only reconnect drivers in the long-run tests demonstrate bounded
attempts without placing that policy in production transport code.

## 11. Adapter boundary / future mapping

```
        future MQTT client (application protocol)
                         |
                         v
                 NetworkTransport  (this module, portable)
                         |
        +----------------+----------------+
        |                                 |
   host fake adapter              future Wi-Fi/TCP adapter
   (test/, host-only)             (later phase; NOT in portable core today)
```

No physical adapter is implemented or linked into the portable core/firmware in
this phase. A future Wi-Fi/TCP adapter must implement the same
`NetworkTransportAdapter` function set (`open/poll/send/recv/close`) and be wired
explicitly.

## 12. Non-blocking guarantees

All public calls return promptly (`O(1)` or a fixed bounded number of adapter
steps). `Run()` performs a bounded drain/fetch per call. **No call ever blocks on
a real network timeout, sleeps, or spins.** `NETWORK_TRANSPORT_BLOCKING_WAIT = NO`.

## 13. Build isolation

`network_transport.{h,c}` is added to the portable core (`CORE_SRC` / `CORE_INC`)
so it is unit-testable on host and available to a future adapter. All Phase 16
host test drivers and the fake adapter live under `test/` and are linked ONLY into
host test executables — never into the STM32 firmware.