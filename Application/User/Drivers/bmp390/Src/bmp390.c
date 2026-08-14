#include "bmp390.h"
#include <string.h>

#ifdef BMP390_UNIT_TEST
/* Host unit-test export. Exposes the static parse/compensate internals so the
   regression suite can verify them against Bosch frozen golden values WITHOUT
   linking any Bosch code into the production driver or the test. */
static void ParseCalibration(const uint8_t *reg, Bmp390QuantizedCalib *q);
static uint32_t Raw24(const uint8_t *p);
static void Compensate(Bmp390QuantizedCalib *q, uint64_t raw_press, int64_t raw_temp,
                       double *temp_out, double *press_out);

void BMP390_UT_ParseCalib(const uint8_t *raw, Bmp390QuantizedCalib *q)
{
    ParseCalibration(raw, q);
}
uint32_t BMP390_UT_Raw24(const uint8_t *p)
{
    return Raw24(p);
}
void BMP390_UT_Compensate(Bmp390QuantizedCalib *q, uint64_t raw_press,
                          int64_t raw_temp, double *temp_out, double *press_out)
{
    Compensate(q, raw_press, raw_temp, temp_out, press_out);
}
#endif

/* --------------------------------------------------------------------------
   Register I/O helpers. The I2cBus uses MemRead/MemWrite (register addressed).
   BMP390 is register-mapped, so I2cBus_ReadMem is appropriate.
-------------------------------------------------------------------------- */

static DriverStatus ReadRegs(const Bmp390 *dev, uint8_t reg, uint8_t *out, size_t len)
{
    if (dev == NULL || dev->bus == NULL || dev->initialized == 0U)
        return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_ReadMem(dev->bus, dev->address, reg, out, len);
}

/* Write a single register using a combined [reg, value] I2C write (register
   addressed, matching I2cBus_ReadMem style). */
static DriverStatus WriteReg(const Bmp390 *dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL || dev->bus == NULL || dev->initialized == 0U)
        return DRIVER_STATUS_INVALID_ARG;
    uint8_t buf[2];
    buf[0] = reg;
    buf[1] = val;
    return I2cBus_Write(dev->bus, dev->address, buf, sizeof(buf));
}

DriverStatus BMP390_Init(Bmp390 *dev, const I2cBus *bus)
{
    if (dev == NULL || bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;
    dev->initialized = 1U;
    dev->address = 0U; /* detected later */
    return DRIVER_STATUS_OK;
}

DriverStatus BMP390_Detect(Bmp390 *dev)
{
    if (dev == NULL || dev->bus == NULL || dev->initialized == 0U)
        return DRIVER_STATUS_INVALID_ARG;

    static const uint16_t prim = (uint16_t)(BMP390_I2C_ADDR_PRIM << 1U); /* 0xEC */
    static const uint16_t sec  = (uint16_t)(BMP390_I2C_ADDR_SEC  << 1U); /* 0xEE */
    const uint16_t addrs[2] = { prim, sec };

    for (int i = 0; i < 2; i++)
    {
        /* device must respond and identify as BMP390 */
        DriverStatus p = I2cBus_Probe(dev->bus, addrs[i]);
        if (p != DRIVER_STATUS_OK)
            continue;

        uint8_t chip = 0;
        DriverStatus r = I2cBus_ReadMem(dev->bus, addrs[i], BMP390_REG_CHIP_ID, &chip, 1U);
        if (r != DRIVER_STATUS_OK)
            continue;

        if (chip == BMP390_CHIP_ID)
        {
            dev->address = addrs[i];
            return DRIVER_STATUS_OK;
        }
        /* Address ACKs but is not a BMP390: continue discovery to secondary. */
    }

    return DRIVER_STATUS_NOT_FOUND;
}

DriverStatus BMP390_ReadChipId(Bmp390 *dev, uint8_t *chip_id)
{
    if (chip_id == NULL) return DRIVER_STATUS_INVALID_ARG;
    return ReadRegs(dev, BMP390_REG_CHIP_ID, chip_id, 1U);
}

/* Parse the 21-byte calibration block exactly as Bosch BMP3_SensorAPI does
   (LSB-first per byte pair; BMP3_CONCAT_BYTES(msb=reg[i+1], lsb=reg[i])). */
static void ParseCalibration(const uint8_t *reg, Bmp390QuantizedCalib *q)
{
    q->par_t1  = (double)(((uint16_t)reg[1] << 8) | reg[0]) / 0.00390625;
    q->par_t2  = (double)(((uint16_t)reg[3] << 8) | reg[2]) / 1073741824.0;
    q->par_t3  = (double)(int8_t)reg[4] / 281474976710656.0;

    q->par_p1  = (double)((int16_t)(((uint16_t)reg[6] << 8) | reg[5]) - (int16_t)16384) / 1048576.0;
    q->par_p2  = (double)((int16_t)(((uint16_t)reg[8] << 8) | reg[7]) - (int16_t)16384) / 536870912.0;
    q->par_p3  = (double)(int8_t)reg[9] / 4294967296.0;
    q->par_p4  = (double)(int8_t)reg[10] / 137438953472.0;

    q->par_p5  = (double)(((uint16_t)reg[12] << 8) | reg[11]) / 0.125;
    q->par_p6  = (double)(((uint16_t)reg[14] << 8) | reg[13]) / 64.0;
    q->par_p7  = (double)(int8_t)reg[15] / 256.0;
    q->par_p8  = (double)(int8_t)reg[16] / 32768.0;

    q->par_p9  = (double)(int16_t)(((uint16_t)reg[18] << 8) | reg[17]) / 281474976710656.0;
    q->par_p10 = (double)(int8_t)reg[19] / 281474976710656.0;
    q->par_p11 = (double)(int8_t)reg[20] / 36893488147419103232.0;

    q->t_lin = 0.0;
}

DriverStatus BMP390_InitCalibration(Bmp390 *dev)
{
    if (dev == NULL) return DRIVER_STATUS_INVALID_ARG;

    uint8_t raw[BMP390_LEN_CALIB_DATA];
    DriverStatus r = ReadRegs(dev, BMP390_REG_CALIB_DATA, raw, sizeof(raw));
    if (r != DRIVER_STATUS_OK)
        return r;

    ParseCalibration(raw, &dev->calib.quantized);
    dev->calib.calibrated = 1U;
    return DRIVER_STATUS_OK;
}

DriverStatus BMP390_ReadError(Bmp390 *dev, uint8_t *err)
{
    if (err == NULL) return DRIVER_STATUS_INVALID_ARG;
    DriverStatus r = ReadRegs(dev, BMP390_REG_ERR, err, 1U);
    if (r != DRIVER_STATUS_OK) return r;
    if ((*err & (BMP390_ERR_FATAL | BMP390_ERR_CMD | BMP390_ERR_CONF)) != 0U)
        return DRIVER_STATUS_DEVICE_ERROR;
    return DRIVER_STATUS_OK;
}

DriverStatus BMP390_ConfigureRoomProfile(Bmp390 *dev)
{
    if (dev == NULL) return DRIVER_STATUS_INVALID_ARG;

    /* v1 room profile (Bosch recommended combos): forced mode, press+temp
       enabled. OSR: pressure x8 (0x03), temperature x1 (0x00). A conservative
       single data sample per forced trigger is used; ODR is not used in forced
       mode. IIR filter coefficient x3 (0x02) for modest smoothing. */
    DriverStatus s;

    /* PWR_CTRL: press_en (bit0)=1, temp_en(bit1)=1, mode bits4-5 = SLEEP now. */
    s = WriteReg(dev, BMP390_REG_PWR_CTRL,
                 (uint8_t)(0x01U | (0x01U << 1) | (BMP390_MODE_SLEEP << 4)));
    if (s != DRIVER_STATUS_OK) return s;

    /* OSR: press_os=0x03 (x8), temp_os=0x02 (x4). */
    s = WriteReg(dev, BMP390_REG_OSR, (uint8_t)(0x03U | (0x02U << 3)));
    if (s != DRIVER_STATUS_OK) return s;

    /* ODR: only meaningful in normal mode; set 25 Hz harmless for forced. */
    s = WriteReg(dev, BMP390_REG_ODR, BMP390_ODR_25_HZ);
    if (s != DRIVER_STATUS_OK) return s;

    /* CONFIG: IIR filter coeff (bits1-3) = 0x02 (x3). */
    s = WriteReg(dev, BMP390_REG_CONFIG, (uint8_t)(0x02U << 1));
    if (s != DRIVER_STATUS_OK) return s;

    return DRIVER_STATUS_OK;
}

DriverStatus BMP390_TriggerMeasurement(Bmp390 *dev)
{
    /* FORCED mode: write mode bits to PWR_CTRL, keeping press_en/temp_en set. */
    uint8_t pwr = (uint8_t)(0x01U | (0x01U << 1)); /* press_en | temp_en */
    return WriteReg(dev, BMP390_REG_PWR_CTRL, (uint8_t)(pwr | (BMP390_MODE_FORCED << 4)));
}

DriverStatus BMP390_ReadStatus(Bmp390 *dev, uint8_t *status)
{
    if (status == NULL) return DRIVER_STATUS_INVALID_ARG;
    return ReadRegs(dev, BMP390_REG_STATUS, status, 1U);
}

/* 24-bit LSB-first raw pressure (data[0..2]) and temperature (data[3..5]). */
static uint32_t Raw24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Official Bosch float point compensation (BMP3_SensorAPI FLOAT path). q->t_lin
   is updated by the temperature step and consumed by the pressure step.

   Temperature clamps are part of the OFFICIAL Bosch algorithm and are retained
   (they are the reference behavior against which golden fixtures were frozen).
   Pressure is NOT clamped here: the Bosch operating range (30000..125000 Pa) is
   used only as DEFENSIVE VALIDATION by the caller. An out-of-range / non-finite
   compensated pressure marks the sample invalid instead of being silently forced
   onto a plausible boundary (Room Sensor barometric policy). */
static void Compensate(Bmp390QuantizedCalib *q, uint64_t raw_press, int64_t raw_temp,
                       double *temp_out, double *press_out)
{
    double partial_data1, partial_data2, partial_data3, partial_data4, partial_out1, partial_out2;

    /* temperature */
    partial_data1 = (double)(raw_temp - q->par_t1);
    partial_data2 = partial_data1 * q->par_t2;
    q->t_lin = partial_data2 + (partial_data1 * partial_data1) * q->par_t3;
    if (q->t_lin < -40.0) q->t_lin = -40.0;
    if (q->t_lin > 85.0)  q->t_lin = 85.0;
    *temp_out = q->t_lin;

    /* pressure */
    partial_data1 = q->par_p6 * q->t_lin;
    partial_data2 = q->par_p7 * q->t_lin * q->t_lin;
    partial_data3 = q->par_p8 * q->t_lin * q->t_lin * q->t_lin;
    partial_out1 = q->par_p5 + partial_data1 + partial_data2 + partial_data3;

    partial_data1 = q->par_p2 * q->t_lin;
    partial_data2 = q->par_p3 * q->t_lin * q->t_lin;
    partial_data3 = q->par_p4 * q->t_lin * q->t_lin * q->t_lin;
    partial_out2 = (double)raw_press *
                   (q->par_p1 + partial_data1 + partial_data2 + partial_data3);

    double p2 = (double)raw_press * (double)raw_press;
    partial_data2 = q->par_p9 + q->par_p10 * q->t_lin;
    partial_data3 = p2 * partial_data2;
    partial_data4 = partial_data3 + ((double)raw_press * (double)raw_press * (double)raw_press) *
                                     q->par_p11;

    *press_out = partial_out1 + partial_out2 + partial_data4;
}

DriverStatus BMP390_ReadSample(Bmp390 *dev, Bmp390Sample *sample)
{
    if (dev == NULL || sample == NULL) return DRIVER_STATUS_INVALID_ARG;
    if (dev->calib.calibrated == 0U) return DRIVER_STATUS_NOT_READY;

    uint8_t data[BMP390_LEN_P_T_DATA];
    DriverStatus r = ReadRegs(dev, BMP390_REG_DATA, data, sizeof(data));
    if (r != DRIVER_STATUS_OK) return r;

    /* The caller MUST have verified both DRDY bits on status before this, so
       pressure and temperature belong to a completed sample pair. Pressure and
       temperature are read in ONE atomic 6-byte transaction, so the sample is
       internally consistent (never new pressure + old temperature). */
    uint32_t raw_p = Raw24(&data[0]);
    uint32_t raw_t = Raw24(&data[3]);

    double temp, press;
    Compensate(&dev->calib.quantized, (uint64_t)raw_p, (int64_t)raw_t, &temp, &press);

    /* Defensive operating range: Bosch documents 30000..125000 Pa and
       -40..85 C. Use as validation only — do NOT clamp a physically impossible
       compensation onto a plausible boundary. A non-finite or out-of-range
       result marks the sample invalid (the runtime/RoomState must not publish
       it as a genuine reading). */
    bool press_finite = (press == press) && (press > -3.0e300) && (press < 3.0e300);
    bool temp_finite  = (temp == temp) && (temp > -3.0e300) && (temp < 3.0e300);
    bool finite = press_finite && temp_finite;
    bool in_range = finite &&
                    (press >= BMP390_MIN_PRES_PA) && (press <= BMP390_MAX_PRES_PA) &&
                    (temp >= -40.0 && temp <= 85.0);

    sample->pressure_pa   = (float)press;
    sample->temperature_c = (float)temp;
    sample->valid = in_range;
    return DRIVER_STATUS_OK;
}

DriverStatus BMP390_SoftReset(Bmp390 *dev)
{
    if (dev == NULL) return DRIVER_STATUS_INVALID_ARG;
    return WriteReg(dev, BMP390_REG_CMD, BMP390_SOFT_RESET_CMD);
}