# SCD41 CO2 Integration Note

## Driver responsibility

`Application/User/Drivers/scd41/` provides a **portable**, low-level SCD41
(Sensirion SCD4x family) driver. It depends **only** on the `I2cBus *`
abstraction — never on the STM32 HAL, App, RoomState, Telemetry, Display or
Command layers. No `void *`, no HAL handles inside the driver.

The driver exposes explicit operations (probe, start/stop periodic measurement,
data-ready query, measurement read). It performs **no blocking delays**; timing
and lifecycle belong to the runtime / App. All protocol constants (I2C address
`0x62`, command IDs, CRC-8) come from the official Sensirion SCD4x datasheet and
the official SCD4x I2C driver.

## Periodic measurement model

The non-blocking runtime (`scd41_runtime.c`) drives the SCD4x periodic
measurement mode, whose signal update interval is 5 seconds. States map onto the
portable `DeviceState` enum: `NOT_FOUND → STARTING → WAITING → READY`, with
`ERROR → RECOVERING` on a bounded failure threshold. The App scheduler calls the
runtime at a poll interval (`SCD41_RUNTIME_POLL_INTERVAL_MS`, 500 ms); the
runtime checks `get_data_ready_status` and only reads when data is ready.

**No busy polling and no blocking.** Each poll advances the state machine from
elapsed time (`Platform_GetTickMs()`) and performs only a short I2C
transaction, so the watchdog is always serviced. No `HAL_Delay`, no spin loops.

## Data-ready semantics

`data_ready = false` is a **valid** result: the runtime stays operational, does
**not** increment error counters, and does **not** overwrite RoomState. Only real
communication / CRC / protocol failures are counted as errors.

## Sample validity and freshness

A measurement is committed to RoomState only when **all three** 16-bit words
(CO2, temperature, RH) pass the SCD4x CRC-8. A CRC failure rejects the **entire**
sample — no partial measurement is committed.

A previously-valid SCD41 value is invalidated after
`SCD41_RUNTIME_STALE_MS` (3 × the 5 s periodic interval = 15 s) without a new
sample. When invalid, CO2 is reported as `--` and serialized with an explicit
invalid state — **never** as `0 ppm`. The numeric last value is retained for
diagnostics, but validity is cleared.

## Recovery

Transient I2C/CRC errors do not permanently mark the sensor missing: the runtime
uses the existing consecutive-error threshold
(`SCD41_RUNTIME_ERROR_THRESHOLD`, 3). After the threshold it enters
`ERROR → RECOVERING` and App re-probes + restarts periodic measurement. A failed
SCD41 never resets the MCU and never stops VEML / display / App. SystemHealth
becomes `DEGRADED` (not fatal) while VEML/display keep operating.

## Source semantics for SCD41 temperature / RH

The SCD41 contains an **internal** temperature/RH sensor used for signal
compensation. These are treated as useful secondary / diagnostic values, **not**
the canonical room T/RH. The RoomState fields are named explicitly
(`scd41_temperature_c`, `scd41_humidity_pct`) so a future dedicated SHT45 can
become the primary environmental T/RH source without renaming or refactoring
the domain. SCD41 T/RH are reported with their own validity flags and must not
be assumed equal in accuracy/placement to a future dedicated sensor.

## Calibration features intentionally deferred

Driver structure leaves room for later addition of official SCD41 commands, but
this first integration does **not** expose user-facing:

- forced recalibration (FRC);
- factory reset;
- temperature-offset calibration;
- altitude / ambient-pressure compensation;
- automatic self-calibration (ASC) policy configuration.

No calibration command runs during boot or SelfTest. SelfTest only performs a
safe ACK/address probe, which does not disturb an active periodic measurement.