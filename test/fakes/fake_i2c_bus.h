#ifndef FAKE_I2C_BUS_H
#define FAKE_I2C_BUS_H

#include <stdint.h>
#include <stddef.h>
#include "i2c_bus.h"

/* Maximum length of a scripted per-address transport-outcome sequence. */
#ifndef FAKE_I2C_SCRIPT_MAX
#define FAKE_I2C_SCRIPT_MAX 64U
#endif

/* Number of simultaneous per-address script slots (distinct devices on a
   shared bus that may fail concurrently). */
#ifndef FAKE_I2C_SCRIPT_SLOTS
#define FAKE_I2C_SCRIPT_SLOTS 8U
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Host fake I2C bus with scripted transaction support.

   Besides the register-mapped VEML/display behavior it ALSO models the
   Sensirion SCD4x command/response style. The SCD4x is a legal-stateful device,
   not merely a status mailbox: it tracks whether it is in IDLE or PERIODIC
   measurement mode and only accepts the commands the datasheet allows in that
   mode. This prevents the class of "fake agrees with production bug" that
   previously hid the retained-periodic-mode failure.

   SCD4x mode legality (datasheet §3.6/Table 9):
     IDLE:     start_periodic (0x21B1) allowed.
     PERIODIC: start_periodic 0x21B1 -> NACK (BUS_ERROR);
               stop_periodic   0x3F86 -> allowed (returns to IDLE after settle).
   Tests script probe success/failure, measurement responses, CRC corruption,
   I2C read/write failure, sensor disappearance and recovery by manipulating the
   fields below and the helper builders. */
typedef enum
{
    FAKE_SCD41_MODE_IDLE = 0,
    FAKE_SCD41_MODE_PERIODIC
} FakeScd41Mode;

typedef struct
{
    /* Modeled SCD4x measurement mode (default IDLE). When PERIODIC, the fake
       rejects start_periodic_measurement with an acknowledge-style failure and
       accepts stop_periodic_measurement (legal during measurement). */
    FakeScd41Mode scd41_mode;

    /* Per-operation results (DRIVER_STATUS_OK by default). */
    DriverStatus write_result;
    DriverStatus read_mem_result;
    DriverStatus read_result;
    DriverStatus probe_result;

    int write_call_count;
    int read_mem_call_count;
    int read_call_count;
    int probe_call_count;

    /* Monotonic tick at which the most recent plain read (I2cBus_Read) was
       performed. Used by timing regressions to prove a two-phase response is
       NOT read before its deadline (the fake stamps Platform_GetTickMs()). */
    uint32_t last_read_tick_ms;

    /* Last 8-bit (left-shifted) device address used across all transactions.
       Lets tests assert a driver addresses the correct on-wire byte (e.g. the
       SCD41 must probe/write/read at the left-shifted 0xC4, not 7-bit 0x62). */
    uint16_t last_addr;

    /* Register-mapped memory (VEML/legacy). */
    uint8_t  regs[256];
    uint16_t last_write_reg;
    uint16_t last_write_value;

    /* Full last-written command/data bytes + length (SCD41 command framing). */
    uint8_t  last_write_data[16];
    size_t   last_write_size;

    /* Last SCD41 command (decoded from last_write_data) for stateful responses:
       GET_DATA_READY (0xE4B8) returns data_ready_word+CRC on a 3-byte read;
       READ_MEASUREMENT (0xEC05) returns read_response. */
    uint16_t last_scd41_cmd;

    /* Ordered log of the last SCD41 2-byte write commands (START_PERIODIC /
       STOP_PERIODIC / GET_DATA_READY / READ_MEASUREMENT), so tests can assert
       the exact protocol sequence across a recovery (e.g. 21B1 3F86 ... 21B1). */
    uint16_t scd41_cmd_log[16];
    int scd41_cmd_log_count;

    /* Scripted data-ready (SCD41 get_data_ready_status returns one word).
       When `data_ready_scripted` is set, a plain read returns this word
       followed by its CRC. */
    uint8_t data_ready_scripted;
    uint16_t data_ready_word;

    /* Scripted plain-read response payload (SCD41 read_measurement: 9 bytes,
       or a 3-byte single-word command response). Reproduced verbatim (no CRC
       injection) so tests can pre-select valid or corrupted bytes. */
    uint8_t  read_response[16];
    size_t   read_response_size;

    /* SHT45 (0x88 wire addr) single-byte command + scripted 6-byte response.
       SHT45 is a single-shot sensor: a 1-byte command write (measure 0xFD etc.),
       then a 6-byte read (T-word+CRC, RH-word+CRC). Independent of the SCD41
       state machine so tests exercise the two sensors independently. */
    uint8_t  sht45_last_cmd;
    uint8_t  sht45_read_response[6];
    bool     sht45_respond;   /* when false, a 6-byte SHT45 read NACKs */
    int      sht45_measure_cmd_count;

    /* BMP390: the flat register map cannot host both VEML's ALS_CONF at reg 0x00
       AND the BMP390 CHIP_ID at reg 0x00. When a BMP390 wire address (0x76/0x77
       left-shifted = 0xEC/0xEE) is read at CHIP_ID (reg 0x00) or CALIB_DATA
       (reg 0x31), serve these dedicated fields instead of the shared regs[], so
       app-level tests can present a BMP390 alongside VEML/display/SCD41/SHT45.
       bmp390_chip_id == 0 means "no BMP390 present" (reads return regs[]). */
    uint16_t bmp390_wire_addr;   /* left-shifted wire address to relay, 0 = disabled */
    uint8_t  bmp390_chip_id;
    uint8_t  bmp390_calib[21];

    /* SGP41 (0xB2 wire addr) fake transport. SGP41 is command/response with
       MSB-first words + CRC. `sgp41_read_response` holds the scripted response
       returned verbatim on a plain read for the SGP41 wire address, so tests
       can pre-select valid or corrupted bytes. */
    uint8_t  sgp41_read_response[16];
    size_t   sgp41_read_response_size;
    DriverStatus sgp41_read_result;   /* per-read result for the SGP41 address */
    int      sgp41_measure_cmd_count;
    uint16_t sgp41_last_cmd;          /* last decoded 2-byte SGP41 command */
    bool     sgp41_absent;            /* when true, SGP41 probe NACKs (absent) */

    /* Shared-bus recovery hook (Phase 6). When `recover_result` is set, the
       bus's recover fn dispatches here and bumps recover_call_count. Enables
       tests to exercise I2cBus_Recover and bus-recovery orchestration. */
    DriverStatus recover_result;
    int          recover_call_count;

    /* ---- Phase 15: deterministic whole-device scenario scripting ---- */

    /* Generic per-address PRESENCE tracking (immediate toggle, time-driven by
       scenario events). `present_addrs[]` lists the addresses that have been
       tracked; `present[]` is the current presence. The probe probe() consults
       this first (a tracked-absent device NACKs), independent of the global
       probe_result and of the SGP41-specific sgp41_absent bool. Default (not
       tracked) behavior is unchanged so existing tests are unaffected. */
    uint16_t present_addrs[8];
    bool     present[8];
    uint8_t  present_count;

    /* Generic PER-ADDRESS TRANSACTION OUTCOME SCRIPTS. Each of the up-to-8
       script slots is bound to one on-wire address. When a slot is active, the
       NEXT `count` I2C operations (probe/write/read/read_mem) to that address
       return the scripted DriverStatus verbatim and DO NOT run the normal device
       logic first — so a scripted BUS_ERROR/TIMEOUT/NOT_FOUND models a real
       transport failure. When a slot's script is exhausted it is cleared and
       normal behavior resumes ("success forever" after N faulted operations).
       Multiple addresses can be scripted simultaneously (e.g. SCD41 + SHT45),
       enabling whole-device shared-bus scenarios. This is the substrate for
       "success x20, timeout x3, success forever" style deterministic scripts. */
    uint16_t  script_addr[FAKE_I2C_SCRIPT_SLOTS];
    bool      script_active[FAKE_I2C_SCRIPT_SLOTS];
    DriverStatus script_status[FAKE_I2C_SCRIPT_SLOTS][FAKE_I2C_SCRIPT_MAX];
    uint32_t    script_count[FAKE_I2C_SCRIPT_SLOTS];
    uint32_t    script_idx[FAKE_I2C_SCRIPT_SLOTS];

    /* BMP390 register-map relay extension: besides CHIP_ID (0x00) and CALIB
       (0x31), also serve STATUS (0x03), ERR (0x02) and the paired pressure/
       temperature sample DATA (0x04..0x09) from dedicated fields so a sustained
       whole-device run can keep VEML (shared regs[0x04..0x05]) and BMP390
       measuring simultaneously without their flat maps colliding. */
    uint8_t  bmp390_status_reg;
    uint8_t  bmp390_err_reg;
    uint8_t  bmp390_p_t_data[6];

    /* SGP41 conditioning vs measure responses, served by last-decoded command
       (conditioning 0x2612 returns CONDITIONING_RESPONSE_BYTES=3; measure
       0x2619 returns MEASURE_RESPONSE_BYTES=6). Keeps a whole-device run that
       exercises both phases scriptable. */
    uint8_t  sgp41_conditioning_response[3];
} FakeI2cBus;

void FakeI2cBus_Init(FakeI2cBus *fake);
void FakeI2cBus_GetBus(I2cBus *bus, FakeI2cBus *fake);

void FakeI2cBus_SetAlsRead(FakeI2cBus *fake, uint16_t raw);

/* SCD4x measurement-mode control (models retained periodic state). */
void FakeI2cBus_SetScd41Mode(FakeI2cBus *fake, FakeScd41Mode mode);

/* Sensirion SCD4x scripting helpers. */

/* Schedule the data-ready plain read response. ready=false clears the
   scripted data-ready word so not-ready reads return a zero word. */
void FakeI2cBus_SetScd41DataReady(FakeI2cBus *fake, bool ready);

/* Inject a raw SCD41 data-ready response as explicit wire bytes (MSB-first
   word + CRC). Used by independent fixed-vector tests; the caller supplies the
   exact bytes, so it does not rely on any encode helper. */
void FakeI2cBus_SetScd41RawDataReady(FakeI2cBus *fake, uint8_t msb, uint8_t lsb);

/* Inject a raw SCD41 measurement response (9 bytes exactly) verbatim. The
   caller supplies the full MSB-first word+CRC triplets, so fixed wire vectors
   can be tested independently of any word encode helper. */
void FakeI2cBus_SetScd41RawRead(FakeI2cBus *fake, const uint8_t raw9[9]);

/* Inject a raw 3-byte single-word command response verbatim (word + CRC),
   used by fixed-vector tests that do not rely on an encode helper. */
void FakeI2cBus_SetRawRead(FakeI2cBus *fake, const uint8_t *data, size_t size);

/* Schedule a 9-byte SCD41 measurement response from raw 16-bit words. Each
   word is followed by its CRC-8. `corrupt_co2/temp/rh` optionally corrupt the
   corresponding CRC byte so a specific word fails validation. */
void FakeI2cBus_SetScd41Measurement(FakeI2cBus *fake,
                                    uint16_t co2,
                                    uint16_t temp_raw,
                                    uint16_t rh_raw,
                                    bool corrupt_co2,
                                    bool corrupt_temp,
                                    bool corrupt_rh);

/* Compute the Sensirion SCD4x CRC-8 for `count` bytes (used to script frames).
   Duplicated from the driver as a test helper; an integration test reads a
   fake-built frame through the real driver to cross-check both copies agree. */
uint8_t FakeI2cBus_Scd41Crc(uint8_t *data, size_t count);

/* Convenience: raw temperature / RH word encodings per SCD4x datasheet.
     temp_raw = (T_c + 45) * 65535 / 175
     rh_raw   = RH_pct       * 65535 / 100 */
uint16_t FakeI2cBus_TempRaw(float temp_c);
uint16_t FakeI2cBus_RhRaw(float rh_pct);

/* SHT45 scripting. `raw6` is the exact on-wire response (T word MSB-first +
   CRC, RH word MSB-first + CRC). `respond` controls whether the fake NACKs the
   6-byte read (sensor busy / conversion in progress). */
void FakeI2cBus_SetSht45Response(FakeI2cBus *fake, const uint8_t raw6[6], bool respond);

/* Present / remove a BMP390 at a given wire address (left-shifted 0xEC/0xEE).
   Relays CHIP_ID (reg 0x00) and CALIB_DATA (reg 0x31) from dedicated fields so
   app-level tests can host a BMP390 alongside VEML/display without the flat
   register map colliding (VEML uses reg 0x00 for ALS_CONF). */
void FakeI2cBus_SetBmp390Present(FakeI2cBus *fake, uint16_t wire_addr,
                                 uint8_t chip_id, const uint8_t calib[21]);
void FakeI2cBus_SetBmp390Absent(FakeI2cBus *fake);

/* SGP41 scripting. `raw` is the exact on-wire response (MSB-first words + CRC)
   returned verbatim on a plain read to the SGP41 wire address. The CRC is NOT
   injected, so tests can supply fixed or corrupted vectors. */
void FakeI2cBus_SetSgp41Response(FakeI2cBus *fake, const uint8_t *raw, size_t size);
void FakeI2cBus_SetSgp41Absent(FakeI2cBus *fake);
/* Decode helper for the SGP41 command word in last_write_data. */
uint16_t FakeI2cBus_Sgp41Cmd(const uint8_t data[2]);

/* ---- Phase 15 scenario-scripting API ---- */

/* Track presence for an on-wire (left-shifted) address. Once tracked, probe()
   reports present/absent from the `present[]` bitmap before falling through to
   the global probe_result. Used to toggle a sensor's physical presence at a
   specific virtual time. */
void FakeI2cBus_SetPresent(FakeI2cBus *fake, uint16_t wire_addr, bool present);

/* Script the next `count` I2C operations to `wire_addr` to return statuses
   verbatim (BUS_ERROR/TIMEOUT/NOT_FOUND/CRC_ERROR/etc.) WITHOUT running the
   normal device logic. After the script is exhausted the device responds
   normally ("success forever"). A NULL/empty script clears any active script. */
void FakeI2cBus_Script(FakeI2cBus *fake, uint16_t wire_addr,
                       const DriverStatus *statuses, uint32_t count);

/* Convenience: script `count` identical statuses then return to normal. */
void FakeI2cBus_ScriptRepeat(FakeI2cBus *fake, uint16_t wire_addr,
                             DriverStatus status, uint32_t count);

/* BMP390 register-map relay: set STATUS (0x03), ERR (0x02) and the paired
   P/T raw sample (DATA 0x04..0x09). Takes effect for the configured BMP390
   wire address set by FakeI2cBus_SetBmp390Present(). */
void FakeI2cBus_SetBmp390Regs(FakeI2cBus *fake, uint8_t status, uint8_t err,
                              const uint8_t p_t_data[6]);

/* Set the SGP41 conditioning response (3 bytes: word + CRC). The fake returns
   it for a plain read after the conditioning command (0x2612) was written. */
void FakeI2cBus_SetSgp41ConditioningResponse(FakeI2cBus *fake, const uint8_t raw[3]);

#ifdef __cplusplus
}
#endif

#endif