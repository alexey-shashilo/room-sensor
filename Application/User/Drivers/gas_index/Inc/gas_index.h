/*
 * Sensirion Gas Index Algorithm (fixpoint) — VOC & NOx Index for SGP40/41.
 *
 * Portable, integer-only (fix16_t = int32_t) implementation of Sensirion's
 * VOC and NOx gas index algorithm. Source: Sensirion/gas-index-algorithm
 * (master), file sensirion_gas_index_algorithm_fixpoint/... , v3.2.0.
 * Authoritative reference: https://github.com/Sensirion/gas-index-algorithm
 *
 * LICENSE: BSD 3-Clause (Sensirion AG, see the BSD notice in this header).
 * The fixed-point arithmetic portion is derived from libfixmath
 * (https://github.com/PetteriAimonen/libfixmath), MIT licensed. Both are
 * permissive and compatible with this repository's MIT license.
 *
 * The implementation is fully self-contained and deterministic: it uses only
 * integer fix16_t arithmetic (the F16() macro folds compile-time float
 * constants), no dynamic allocation, no STM32 HAL, no filesystem, and no
 * runtime floating point.
 *
 * THE ALGORITHM ASSUMES A CONSTANT ~1 Hz SAMPLING INTERVAL. The caller MUST
 * call GasIndexAlgorithm_process() once per second (not at an arbitrary App
 * cadence). If samples are skipped, the algorithm effectively advances as if
 * the skipped time steps with no raw update occurred — it must not be called
 * back-to-back more frequently than once per ~1 s.
 */

#ifndef GAS_INDEX_H
#define GAS_INDEX_H

#include <stdint.h>

/* The fix16_t fixed-point arithmetic was originally created by
 * https://github.com/PetteriAimonen/libfixmath (MIT). Reproduced here as part
 * of the Sensirion Gas Index Algorithm (BSD 3-Clause). */
typedef int32_t fix16_t;

#define F16(x) \
    ((fix16_t)(((x) >= 0) ? ((x)*65536.0 + 0.5) : ((x)*65536.0 - 0.5)))

#if defined(__cplusplus)
#error "gas_index is C11 only"
#endif

#if __STDC_VERSION__ >= 199901L
#include <stdbool.h>
#endif

/* Should be set by the building toolchain */
#ifndef LIBRARY_VERSION_NAME
#define LIBRARY_VERSION_NAME "3.2.0"
#endif

#define GasIndexAlgorithm_ALGORITHM_TYPE_VOC (0)
#define GasIndexAlgorithm_ALGORITHM_TYPE_NOX (1)
#define GasIndexAlgorithm_SAMPLING_INTERVAL (1.)
#define GasIndexAlgorithm_INITIAL_BLACKOUT (45.)
#define GasIndexAlgorithm_INDEX_GAIN (230.)
#define GasIndexAlgorithm_SRAW_STD_INITIAL (50.)
#define GasIndexAlgorithm_SRAW_STD_BONUS_VOC (220.)
#define GasIndexAlgorithm_SRAW_STD_NOX (2000.)
#define GasIndexAlgorithm_TAU_MEAN_HOURS (12.)
#define GasIndexAlgorithm_TAU_VARIANCE_HOURS (12.)
#define GasIndexAlgorithm_TAU_INITIAL_MEAN_VOC (20.)
#define GasIndexAlgorithm_TAU_INITIAL_MEAN_NOX (1200.)
#define GasIndexAlgorithm_INIT_DURATION_MEAN_VOC ((3600. * 0.75))
#define GasIndexAlgorithm_INIT_DURATION_MEAN_NOX ((3600. * 4.75))
#define GasIndexAlgorithm_INIT_TRANSITION_MEAN (0.01)
#define GasIndexAlgorithm_TAU_INITIAL_VARIANCE (2500.)
#define GasIndexAlgorithm_INIT_DURATION_VARIANCE_VOC ((3600. * 1.45))
#define GasIndexAlgorithm_INIT_DURATION_VARIANCE_NOX ((3600. * 5.70))
#define GasIndexAlgorithm_INIT_TRANSITION_VARIANCE (0.01)
#define GasIndexAlgorithm_GATING_THRESHOLD_VOC (340.)
#define GasIndexAlgorithm_GATING_THRESHOLD_NOX (30.)
#define GasIndexAlgorithm_GATING_THRESHOLD_INITIAL (510.)
#define GasIndexAlgorithm_GATING_THRESHOLD_TRANSITION (0.09)
#define GasIndexAlgorithm_GATING_VOC_MAX_DURATION_MINUTES ((60. * 3.))
#define GasIndexAlgorithm_GATING_NOX_MAX_DURATION_MINUTES ((60. * 12.))
#define GasIndexAlgorithm_GATING_MAX_RATIO (0.3)
#define GasIndexAlgorithm_SIGMOID_L (500.)
#define GasIndexAlgorithm_SIGMOID_K_VOC (-0.0065)
#define GasIndexAlgorithm_SIGMOID_X0_VOC (213.)
#define GasIndexAlgorithm_SIGMOID_K_NOX (-0.0101)
#define GasIndexAlgorithm_SIGMOID_X0_NOX (614.)
#define GasIndexAlgorithm_VOC_INDEX_OFFSET_DEFAULT (100.)
#define GasIndexAlgorithm_NOX_INDEX_OFFSET_DEFAULT (1.)
#define GasIndexAlgorithm_LP_TAU_FAST (20.0)
#define GasIndexAlgorithm_LP_TAU_SLOW (500.0)
#define GasIndexAlgorithm_LP_ALPHA (-0.2)
#define GasIndexAlgorithm_VOC_SRAW_MINIMUM (20000)
#define GasIndexAlgorithm_NOX_SRAW_MINIMUM (10000)
#define GasIndexAlgorithm_PERSISTENCE_UPTIME_GAMMA ((3. * 3600.))
#define GasIndexAlgorithm_TUNING_INDEX_OFFSET_MIN (1)
#define GasIndexAlgorithm_TUNING_INDEX_OFFSET_MAX (250)
#define GasIndexAlgorithm_TUNING_LEARNING_TIME_OFFSET_HOURS_MIN (1)
#define GasIndexAlgorithm_TUNING_LEARNING_TIME_OFFSET_HOURS_MAX (1000)
#define GasIndexAlgorithm_TUNING_LEARNING_TIME_GAIN_HOURS_MIN (1)
#define GasIndexAlgorithm_TUNING_LEARNING_TIME_GAIN_HOURS_MAX (1000)
#define GasIndexAlgorithm_TUNING_GATING_MAX_DURATION_MINUTES_MIN (0)
#define GasIndexAlgorithm_TUNING_GATING_MAX_DURATION_MINUTES_MAX (3000)
#define GasIndexAlgorithm_TUNING_STD_INITIAL_MIN (10)
#define GasIndexAlgorithm_TUNING_STD_INITIAL_MAX (5000)
#define GasIndexAlgorithm_TUNING_GAIN_FACTOR_MIN (1)
#define GasIndexAlgorithm_TUNING_GAIN_FACTOR_MAX (1000)
#define GasIndexAlgorithm_MEAN_VARIANCE_ESTIMATOR__GAMMA_SCALING (64.)
#define GasIndexAlgorithm_MEAN_VARIANCE_ESTIMATOR__ADDITIONAL_GAMMA_MEAN_SCALING \
    (8.)
#define GasIndexAlgorithm_MEAN_VARIANCE_ESTIMATOR__FIX16_MAX (32767.)

/**
 * Struct to hold all parameters and states of the gas algorithm.
 */
typedef struct {
    int32_t mAlgorithm_Type;
    fix16_t mIndex_Offset;
    int32_t mSraw_Minimum;
    fix16_t mGating_Max_Duration_Minutes;
    fix16_t mInit_Duration_Mean;
    fix16_t mInit_Duration_Variance;
    fix16_t mGating_Threshold;
    fix16_t mIndex_Gain;
    fix16_t mTau_Mean_Hours;
    fix16_t mTau_Variance_Hours;
    fix16_t mSraw_Std_Initial;
    fix16_t mUptime;
    fix16_t mSraw;
    fix16_t mGas_Index;
    bool m_Mean_Variance_Estimator___Initialized;
    fix16_t m_Mean_Variance_Estimator___Mean;
    fix16_t m_Mean_Variance_Estimator___Sraw_Offset;
    fix16_t m_Mean_Variance_Estimator___Std;
    fix16_t m_Mean_Variance_Estimator___Gamma_Mean;
    fix16_t m_Mean_Variance_Estimator___Gamma_Variance;
    fix16_t m_Mean_Variance_Estimator___Gamma_Initial_Mean;
    fix16_t m_Mean_Variance_Estimator___Gamma_Initial_Variance;
    fix16_t m_Mean_Variance_Estimator__Gamma_Mean;
    fix16_t m_Mean_Variance_Estimator__Gamma_Variance;
    fix16_t m_Mean_Variance_Estimator___Uptime_Gamma;
    fix16_t m_Mean_Variance_Estimator___Uptime_Gating;
    fix16_t m_Mean_Variance_Estimator___Gating_Duration_Minutes;
    fix16_t m_Mean_Variance_Estimator___Sigmoid__K;
    fix16_t m_Mean_Variance_Estimator___Sigmoid__X0;
    fix16_t m_Mox_Model__Sraw_Std;
    fix16_t m_Mox_Model__Sraw_Mean;
    fix16_t m_Sigmoid_Scaled__K;
    fix16_t m_Sigmoid_Scaled__X0;
    fix16_t m_Sigmoid_Scaled__Offset_Default;
    fix16_t m_Adaptive_Lowpass__A1;
    fix16_t m_Adaptive_Lowpass__A2;
    bool m_Adaptive_Lowpass___Initialized;
    fix16_t m_Adaptive_Lowpass___X1;
    fix16_t m_Adaptive_Lowpass___X2;
    fix16_t m_Adaptive_Lowpass___X3;
} GasIndexAlgorithmParams;

/**
 * Initialize the gas index algorithm parameters for the specified algorithm
 * type and reset its internal states. Call this once at the beginning.
 * @param params            Pointer to the GasIndexAlgorithmParams struct
 * @param algorithm_type    GasIndexAlgorithm_ALGORITHM_TYPE_VOC or
 *                          GasIndexAlgorithm_ALGORITHM_TYPE_NOX
 */
void GasIndexAlgorithm_init(GasIndexAlgorithmParams *params,
                            int32_t algorithm_type);

/**
 * Reset the internal states of the gas index algorithm. Previously set tuning
 * parameters are preserved. Call this when resuming operation after a
 * measurement interruption.
 */
void GasIndexAlgorithm_reset(GasIndexAlgorithmParams *params);

/**
 * Get current algorithm states for resume-after-short-interruption
 * (VOC only, after >= 3 hours of continuous operation).
 */
void GasIndexAlgorithm_get_states(const GasIndexAlgorithmParams *params,
                                  int32_t *state0, int32_t *state1);

/**
 * Restore previously retrieved algorithm states (VOC only).
 */
void GasIndexAlgorithm_set_states(GasIndexAlgorithmParams *params,
                                  int32_t state0, int32_t state1);

/**
 * Customize the gas index algorithm. Call once after GasIndexAlgorithm_init()
 * and before GasIndexAlgorithm_set_states(), if desired.
 */
void GasIndexAlgorithm_set_tuning_parameters(
    GasIndexAlgorithmParams *params, int32_t index_offset,
    int32_t learning_time_offset_hours, int32_t learning_time_gain_hours,
    int32_t gating_max_duration_minutes, int32_t std_initial,
    int32_t gain_factor);

/**
 * Get the current tuning parameters.
 */
void GasIndexAlgorithm_get_tuning_parameters(
    const GasIndexAlgorithmParams *params, int32_t *index_offset,
    int32_t *learning_time_offset_hours, int32_t *learning_time_gain_hours,
    int32_t *gating_max_duration_minutes, int32_t *std_initial,
    int32_t *gain_factor);

/**
 * Calculate the gas index value from the raw sensor value.
 *
 * MUST be called at a constant ~1 Hz sampling interval (once per second), the
 * interval the algorithm assumes (GasIndexAlgorithm_SAMPLING_INTERVAL).
 *
 * @param params      Pointer to the GasIndexAlgorithmParams struct
 * @param sraw        Raw value from the SGP4x sensor
 * @param gas_index   Calculated gas index value from the raw sensor value.
 *                    Zero during initial blackout period and 1..500 afterwards
 */
void GasIndexAlgorithm_process(GasIndexAlgorithmParams *params, int32_t sraw,
                               int32_t *gas_index);

#endif /* GAS_INDEX_H */