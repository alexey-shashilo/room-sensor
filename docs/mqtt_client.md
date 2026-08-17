# MQTT Client (Phase 17)

A portable MQTT 3.1.1 client foundation that runs on top of the existing
`NetworkTransport`. It is a *protocol/client* layer only: byte-stream
transport, reconnect policy, and application scheduling all live elsewhere.

## Version and scope

- MQTT protocol: **3.1.1** (`MQTT_PROTOCOL_LEVEL = 4`).
- QoS 0: **supported**.
- QoS 1: **supported** (single bounded inflight slot).
- QoS 2: **not supported** (inbound QoS 2 PUBLISH or PUBREC/PUBREL/PUBCOMP are
  rejected as protocol errors).
- Retained-publish handling: the RETAIN flag is parsed, but no retained-message
  policy is implemented (Phase 17).

## Supported / unsupported packets

| Direction | Supported | Not supported |
|-----------|-----------|---------------|
| Client → Broker | CONNECT, PUBLISH (QoS0/1), PUBACK, SUBSCRIBE, PINGREQ, DISCONNECT | QoS2 (PUBREC/PUBREL/PUBCOMP), UNSUBSCRIBE, AUTH, MQTT5 |
| Broker → Client | CONNACK, PUBLISH (QoS0/1), PUBACK, SUBACK, PINGRESP | PUBREC/PUBREL/PUBCOMP, UNSUBACK, AUTH, MQTT5 properties |

## Buffer limits

| Bound | Value | Derivation |
|-------|-------|-----------|
| `MQTT_MAX_PACKET_SIZE` | 2304 B | telemetry payload (2048) + topic + QoS1 framing + fixed header + RL varint |
| `MQTT_MAX_PAYLOAD_SIZE` | 2048 B | equals `TELEMETRY_SERIALIZED_MAX_SIZE` |
| `MQTT_MAX_TOPIC_LENGTH` | 64 B | short telemetry/command topics |
| `MQTT_MAX_CLIENT_ID_LENGTH` | 64 B | spec-recommended bound |
| `MQTT_MAX_INFLIGHT_QOS1` | 1 | single QoS1 transaction slot |

All buffers are compile-time allocated inside `MqttClient`. **No dynamic
allocation, no unbounded queues.**

## Memory ownership

- Client ID is copied into client-owned storage; no borrowed pointer retained.
- Inbound PUBLISH topic/payload are **borrowed** from the client parser buffer and
  are valid only during the publish callback. The callback MUST copy what it
  needs (the Phase 17 command test does exactly this).
- `NetworkTransport` owns its own TX/RX rings; MQTT owns one outbound packet
  buffer and one inbound parser buffer.

## State machine

```
 DISCONNECTED --MqttClient_Connect--> CONNECTING_TRANSPORT
      ^                                  |
      |                            transport connect + CONNECT sent
      |                                  v
      +-- MqttClient_Disconnect      WAIT_CONNACK
      +-- teardown                       | (CONNACK accepted)
      |                                  v
      +-------------------------------- CONNECTED
      |                                  |
      | (terminal error: protocol error, | MqttClient_Disconnect -> DISCONNECTING
      |  transport error, timeout,       |   -> DISCONNECTED
      |  broker refusal)                 |
      v                                  v
      ERROR  --MqttClient_Disconnect--> DISCONNECTED
```

Connection-scoped state (TX/RX parser, QoS1 inflight, pending subscription,
deferred PUBACK, ping, timers) is cleared at every epoch boundary by a single
`ResetConnectionEpoch()`; diagnostic counters and persistent configuration
(client ID, keepalive) are preserved.

## NetworkTransport boundary

- `MQTT != transport`: `NetworkTransport` provides the byte-stream connection
  mechanism; MQTT provides framing, protocol state, keepalive, packet IDs, and
  QoS semantics.
- `MQTT != reconnect policy`: MQTT never reconnects automatically. On a terminal
  error it renders `ERROR`; an upper policy layer decides when to call
  `MqttClient_Disconnect` + `MqttClient_Connect` again.
- `MQTT != application telemetry scheduler`: MQTT does not schedule telemetry.
- `MQTT != command authorization`: MQTT only extracts inbound payload bytes.

## QoS 0 semantics

`MqttClient_PublishQos0` completes when the packet bytes are **locally accepted**
into the `NetworkTransport` (into its TX ring). This is NOT broker delivery and
NOT peer acknowledgement.

## QoS 1 semantics (outbound)

One inflight slot. `PublishQos1` allocates a packet ID (1..65535, wraps to 1,
never 0) and completes only on a matching PUBACK. A second QoS1 publish while
the slot is occupied returns `MQTT_BUSY`.

### QoS1 reconnect-loss policy

`STALE_QOS1_REPLAYED_AFTER_RECONNECT = NO`. On connection loss (terminal
transport error / explicit disconnect / protocol-fatal error), the connection-
scoped inflight QoS1 transaction is discarded. It is never replayed or
re-transmitted after reconnect. On Clean Session = 1 with no persistence and no
offline queue, QoS1 does not provide durable exactly-once application delivery;
the upper layer decides whether to regenerate application data.

## Packet IDs

- QoS1 PUBLISH and SUBSCRIBE use uint16 packet IDs.
- `0` is invalid; IDs wrap 65535 → 1 and never produce 0.
- IDs are per-client instance (not global).

## Keepalive

- `MQTT_KEEPALIVE_DEFAULT_S = 60`.
- If no outbound MQTT packet is accepted within the keepalive interval, a
  PINGREQ is issued.
- A PINGRESP must arrive within `MQTT_PINGRESP_TIMEOUT_MS` (20000 ms) or the
  client enters `ERROR` (timeout).
- Continuous traffic suppresses unnecessary pings (no ping storms).
- All timing uses wrap-safe uint32 arithmetic (`MQTT_KEEPALIVE_UINT32_WRAP_SAFE
  = YES`).

## Malformed-packet / terminal-error policy

- Malformed fixed headers (illegal flags, QoS==3), bad Remaining Length
  (continuation beyond 4 bytes), oversized packets (> `MQTT_MAX_PACKET_SIZE`),
  truncated packets, and unexpected packets in the current state are all
  rejected deterministically.
- On a packet-fatal or connection-fatal error the client enters `ERROR`
  (`MQTT_CONNECTION_STATE_LEAKS_ACROSS_RECONNECT = NO`).
- Oversized packets are rejected without allocation
  (`MQTT_OVERSIZED_PACKET_ALLOCATES_MEMORY = NO`).

## Error model

`MqttStatus`: `OK`, `WOULD_BLOCK`, `IN_PROGRESS`, `INVALID_ARG`,
`NOT_CONNECTED`, `BUSY`, `PROTOCOL_ERROR`, `PACKET_TOO_LARGE`,
`TRANSPORT_ERROR`, `TIMEOUT`, `BROKER_REFUSED`.

## RX fragmentation / TX partial acceptance

- RX: fully byte-stream safe. The client accumulates a packet across any number
  of `NetworkTransport_Receive` results (`MQTT_RX_FRAGMENTATION_SAFE = YES`);
  no pointer references temporary transport storage after a call.
- TX: respects `NetworkTransport` local acceptance. A packet is fed over
  multiple `Send` calls; `MQTT_PARTIAL_TX_DUPLICATES_BYTES = NO` and
  `MQTT_PARTIAL_TX_LOSES_BYTES = NO`.

## Physical adapter / TLS

- Physical network adapter: **NOT implemented** (Phase 17 runs against a
  host fake).
- TLS/certificates: **NOT implemented**.
- No credentials or secrets are used.

## Build isolation

MQTT production sources are compiled into the portable core/firmware but
**NOT activated from App** (`MQTT_ACTIVE_IN_APP = NO`): no broker connection is
attempted at boot and no NetworkTransport adapter is required at boot. All
Phase 17 test code and the fake peer live under `test/` and never enter
firmware (`PHASE17_TEST_CODE_LINKED_INTO_FIRMWARE = NO`).