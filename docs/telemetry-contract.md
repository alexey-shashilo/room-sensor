# Telemetry Contract v1

## Schema

- **Version:** 1 (`TELEMETRY_SCHEMA_VERSION`)
- **Format:** JSON (UTF-8 without BOM)
- **Maximum payload size:** 1024 bytes (`TELEMETRY_SERIALIZED_MAX_SIZE`)
- **Transport:** independent — currently UART debug, future MQTT/Wi-Fi

## Top-Level Fields

| Field | Type | Always present | Description |
|-------|------|---------------|-------------|
| `schema` | `uint32` | yes | Schema version (currently 1) |
| `device_id` | `string` | yes | 36-char UUID format (`xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`) |
| `seq` | `uint32` | yes | Monotonically increasing per-session sequence number |
| `uptime_ms` | `uint32` | yes | Milliseconds since boot |
| `captured_at_ms` | `uint32` | yes | Monotonic time at snapshot creation |
| `health` | `string` | yes | "booting", "ok", "degraded", "fault" |
| `room` | `object` | yes | Room measurements |
| `session` | `uint32` | yes | Session number (uptime_minutes) |

## Room Object Fields

| Field | Type | Description |
|-------|------|-------------|
| `illuminance_lux` | `object` | Ambient light measurement |

Each measurement object:

| Sub-field | Type | Description |
|-----------|------|-------------|
| `value` | `float` | Numeric value (absent when `state` is not "valid") |
| `state` | `string` | "valid", "invalid", or "stale" |

## Device ID Format

The 16-byte device UUID is serialized as:

```
xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

Lowercase hex, hyphenated in 8-4-4-4-12 groups. Based on STM32 unique ID plus runtime seed at first boot.

## Sequence Semantics

- Generated per-snapshot
- Starts at 1 after boot
- Not persisted across reboots
- Server can detect gaps within a session
- Session identified by `device_id` + boot moment

## Health States

| Enum | Wire string | Meaning |
|------|-------------|---------|
| `SYSTEM_HEALTH_BOOTING` | `booting` | Startup phase |
| `SYSTEM_HEALTH_OK` | `ok` | All expected devices operational |
| `SYSTEM_HEALTH_DEGRADED` | `degraded` | One or more optional devices unavailable |
| `SYSTEM_HEALTH_FAULT` | `fault` | Core platform failure |

## Measurement States

| String | Meaning |
|--------|---------|
| `valid` | Fresh, current measurement |
| `invalid` | Sensor unavailable, transient error, or NaN |
| `stale` | Measurement exists but may be outdated (future) |

## Units

| Measurement | Unit | Decimal places |
|-------------|------|----------------|
| illuminance | lux | 1 |
| temperature | °C | 2 (future) |
| humidity | % RH | 1 (future) |
| pressure | hPa | 1 (future) |
| CO2 | ppm | 0 (future) |
| VOC | index | 2 (future) |
| presence | boolean | — (future) |

## Timestamps

All timestamps are based on `Platform_GetTickMs()` — millisecond-resolution monotonic time since boot. They are NOT Unix/POSIX timestamps. After network time synchronization, a UTC timestamp field may be added as an optional extension.

## Latest-Value-Wins

- The device maintains exactly **one** pending telemetry snapshot.
- If a new snapshot is created while the previous one is still unsent, it replaces it unconditionally.
- When communication becomes available, only the latest snapshot is sent.
- No outbox, no queue, no retry of intermediate values.

## Offline Behavior

- Telemetry snapshots are generated periodically regardless of transport availability.
- If transport is unavailable, snapshots are discarded after being replaced by the next one.
- No telemetry is stored in Flash.
- Sensor measurements, display, and all other system functions continue normally.

## Compatibility Policy

- New optional fields may be added without changing `schema`.
- Breaking semantic changes require incrementing `TELEMETRY_SCHEMA_VERSION`.
- Servers must ignore unknown fields.
- Fields must not be repurposed — a removed field must not be reused with different semantics.