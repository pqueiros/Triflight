/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <math.h>
#include <float.h>
#include "platform.h"
#include "debug.h"

#include "common/maths.h"
#include "common/axis.h"
#include "common/color.h"
#include "common/encoding.h"
#include "common/utils.h"
#include "common/filter.h"

#include "drivers/gpio.h"
#include "drivers/sensor.h"
#include "drivers/system.h"
#include "drivers/serial.h"
#include "drivers/compass.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"
#include "drivers/accgyro.h"
#include "drivers/light_led.h"
#include "drivers/adc.h"

#include "sensors/sensors.h"
#include "sensors/boardalignment.h"
#include "sensors/sonar.h"
#include "sensors/compass.h"
#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/gyro.h"
#include "sensors/battery.h"

#include "io/beeper.h"
#include "io/display.h"
#include "io/escservo.h"
#include "io/rc_controls.h"
#include "io/gimbal.h"
#include "io/gps.h"
#include "io/ledstrip.h"
#include "io/serial.h"
#include "io/serial_cli.h"
#include "io/serial_msp.h"
#include "io/statusindicator.h"

#include "rx/rx.h"
#include "rx/msp.h"

#include "telemetry/telemetry.h"

#include "flight/mixer.h"
#include "flight/mixer_tricopter.h"
#include "flight/altitudehold.h"
#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/navigation.h"

#include "config/runtime_config.h"
#include "config/config.h"
#include "config/config_profile.h"
#include "config/config_master.h"

#include "mw.h"

#ifdef USE_SERVOS
#define TRI_TAIL_SERVO_ANGLE_MID (900)
#define TRI_YAW_FORCE_CURVE_SIZE (100)
#define TRI_TAIL_SERVO_MAX_ANGLE (500)

#define SERVO_CALIB_NUM_OF_MEAS  (5)

#define TT_CALIB_I_TARGET       (8)
#define TT_CALIB_I_LARGE_INCREMENT_LIMIT (10)

#define IsDelayElapsed_us(timestamp_us, delay_us) ((uint32_t)(micros() - timestamp_us) >= delay_us)
#define IsDelayElapsed_ms(timestamp_ms, delay_ms) ((uint32_t)(millis() - timestamp_ms) >= delay_ms)

typedef enum {
    TT_IDLE = 0,
    TT_WAIT,
    TT_ACTIVE,
    TT_WAIT_FOR_DISARM,
    TT_DONE,
    TT_FAIL,
} tailTuneState_e;

typedef enum {
    SS_IDLE = 0,
    SS_SETUP,
    SS_CALIB,
} servoSetupState_e;

typedef enum {
    SS_C_IDLE = 0,
    SS_C_CALIB_MIN_MID_MAX,
    SS_C_CALIB_SPEED,
} servoSetupCalibState_e;

typedef enum {
    SS_C_MIN = 0,
    SS_C_MID,
    SS_C_MAX,
} servoSetupCalibSubState_e;

typedef enum {
    TT_MODE_NONE = 0,
    TT_MODE_THRUST_TORQUE,
    TT_MODE_SERVO_SETUP,
} tailtuneMode_e;

typedef struct tailTune_s {
    tailtuneMode_e mode;
    struct thrustTorque_t
    {
        tailTuneState_e state;
        uint32_t timestamp_ms;
        uint32_t lastAdjTime_ms;
        struct servoAvgAngle_t
        {
            float sum;
            uint16_t numOf;
        } servoAvgAngle;
    }tt;
    struct servoSetup_t
    {
        servoSetupState_e state;
        float servoVal;
        int16_t *pLimitToAdjust;
        struct servoCalib_t
        {
            _Bool done;
            _Bool waitingServoToStop;
            servoSetupCalibState_e state;
            servoSetupCalibSubState_e subState;
            uint32_t timestamp_ms;
            struct average_t
            {
                uint16_t *pCalibConfig;
                uint32_t sum;
                uint16_t numOf;
            } avg;
        }cal;
    }ss;
} tailTune_t;

#endif

#ifdef USE_SERVOS
extern float dT;
extern master_t masterConfig;

static tailTune_t tailTune = {.mode = TT_MODE_NONE};
static int32_t tailServoMaxYawForce = 0;
 float tailServoThrustFactor = 0;
static int16_t tailServoMaxAngle = 0;
static int16_t tailServoSpeed = 0;

struct triServoAngle_t
{
    float virtual;
    float feedback;
} tailServoAngle = {.virtual = TRI_TAIL_SERVO_ANGLE_MID / 10.0f, .feedback = TRI_TAIL_SERVO_ANGLE_MID / 10.0f};
static int32_t yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE];
static int16_t tailMotorPitchZeroAngle;
static int16_t tailMotorAccelerationDelay_ms = 30;
static int16_t tailMotorDecelerationDelay_ms = 100;
static int16_t tailMotorAccelerationDelay_angle;
static int16_t tailMotorDecelerationDelay_angle;

static servoParam_t * gpTailServoConf;
static int16_t *gpTailServo;
static mixerConfig_t *gpMixerConfig;

static uint16_t tailServoADC = 0;

static void initCurves();
static uint16_t getServoValueAtAngle(servoParam_t * servoConf, uint16_t angle);
static float getPitchCorrectionAtTailAngle(float angle, float thrustFactor);
static uint16_t getAngleFromYawCurveAtForce(int32_t force);
static uint16_t getServoAngle(servoParam_t * servoConf, uint16_t servoValue);
static uint16_t getPitchCorrectionMaxPhaseShift(int16_t servoAngle,
        int16_t servoSetpointAngle,
        int16_t motorAccelerationDelayAngle,
        int16_t motorDecelerationDelayAngle,
        int16_t motorDirectionChangeAngle);
static uint16_t getLinearServoValue(servoParam_t *servoConf, uint16_t servoValue);
static void virtualServoStep(float dT, servoParam_t *servoConf, uint16_t servoValue);
static void tailTuneModeThrustTorque(struct thrustTorque_t *pTT, throttleStatus_e throttleStatus);
static void tailTuneModeServoSetup(struct servoSetup_t *pSS, servoParam_t *pServoConf, int16_t *pServoVal);
static void triTailTuneStep(servoParam_t *pServoConf, int16_t *pServoVal);
static void updateServoAngles(void);
#endif

void triInitMixer(servoParam_t *pTailServoConfig,
        int16_t *pTailServo,
        mixerConfig_t *pMixerConfig)
{
    gpTailServoConf = pTailServoConfig;
    gpTailServo = pTailServo;
    tailServoThrustFactor = pMixerConfig->tri_tail_motor_thrustfactor / 10.0f;
    tailServoMaxAngle = pMixerConfig->tri_servo_angle_at_max;
    tailServoSpeed = pMixerConfig->tri_tail_servo_speed;
    gpMixerConfig = pMixerConfig;

    initCurves();
}

static void initCurves()
{
    // DERIVATE(1/(sin(x)-cos(x)/tailServoThrustFactor)) = 0
    // Multiplied by 10 to get decidegrees
    tailMotorPitchZeroAngle = 10.0f * 2.0f * (atanf(((sqrtf(tailServoThrustFactor * tailServoThrustFactor + 1) + 1) / tailServoThrustFactor)));

    tailMotorAccelerationDelay_angle = 10.0f * (tailMotorAccelerationDelay_ms / 1000.0f) * tailServoSpeed;
    tailMotorDecelerationDelay_angle = 10.0f * (tailMotorDecelerationDelay_ms / 1000.0f) * tailServoSpeed;

    const int16_t minAngle = TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle;
    const int16_t maxAngle = TRI_TAIL_SERVO_ANGLE_MID + tailServoMaxAngle;
    int32_t maxNegForce = 0;
    int32_t maxPosForce = 0;

    int16_t angle = TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE;
    for (int32_t i = 0; i < TRI_YAW_FORCE_CURVE_SIZE; i++)
    {
        const float angleRad = DEGREES_TO_RADIANS(angle / 10.0f);
        yawForceCurve[i] = 1000.0f * (-tailServoThrustFactor * cosf(angleRad) - sinf(angleRad)) * getPitchCorrectionAtTailAngle(angleRad, tailServoThrustFactor);
        // Only calculate the top forces in the configured angle range
        if ((angle >= minAngle) && (angle <= maxAngle))
        {
            maxNegForce = MIN(yawForceCurve[i], maxNegForce);
            maxPosForce = MAX(yawForceCurve[i], maxPosForce);
        }
        angle += 10;
    }

    tailServoMaxYawForce = MIN(ABS(maxNegForce), ABS(maxPosForce));
}

float triGetCurrentActiveServoAngle(void)
{
    if (gpMixerConfig->tri_servo_feedback_mode == TRI_SERVO_FEEDBACK)
    {
        return triGetCurrentServoAngle(TRI_SERVO_FEEDBACK);
    }
    else
    {
        return triGetCurrentServoAngle(TRI_SERVO_VIRTUAL);
    }
}

float triGetCurrentServoAngle(triServoType_e servoType)
{
    if (servoType == TRI_SERVO_VIRTUAL)
    {
        return tailServoAngle.virtual;
    }
    else
    {
        return tailServoAngle.feedback;
    }
}

static uint16_t getLinearServoValue(servoParam_t *servoConf, uint16_t servoValue)
{
    const int16_t servoMid = servoConf->middle;
    // First find the yaw force at given servo value from a linear curve
    const int16_t servoRange = (servoValue < servoMid) ? servoMid - servoConf->min : servoConf->max - servoMid;
    const int32_t linearYawForceAtValue = (int32_t)(tailServoMaxYawForce) * (servoValue - servoMid) / servoRange;
    const int16_t correctedAngle = getAngleFromYawCurveAtForce(linearYawForceAtValue);
    return getServoValueAtAngle(servoConf, correctedAngle);
}

int16_t toggleServoReverse( const servoParam_t *servoConf, int16_t servoValue)
{
    //MIXER_TRI is only supposed to work with reserve in STABILIZED_YAW right?
    return servoDirection(SERVO_RUDDER, INPUT_STABILIZED_YAW)*(servoValue - servoConf->middle) + servoConf->middle;
}

void triServoMixer()
{
    static filterStatePt1_t feedbackFilter;
    tailServoADC = filterApplyPt1(adcGetChannel(ADC_EXTERNAL1), &feedbackFilter, 70, dT);

    //normalize reverse
    *gpTailServo = toggleServoReverse(gpTailServoConf, *gpTailServo);

    if (ARMING_FLAG(ARMED))
    {
        *gpTailServo = getLinearServoValue(gpTailServoConf, *gpTailServo);
    }

    triTailTuneStep(gpTailServoConf, gpTailServo);

    updateServoAngles();
    //restore reverse
    *gpTailServo = toggleServoReverse(gpTailServoConf, *gpTailServo);
}

int16_t triGetMotorCorrection(uint8_t motorIndex)
{
    uint16_t correction = 0;
    if (motorIndex == 0)
    {
        // Adjust tail motor speed based on servo angle. Check how much to adjust speed from pitch force curve based on servo angle.
        // Take motor speed up lag into account by shifting the phase of the curve
        // Not taking into account the motor braking lag (yet)
        const int16_t servoAngle = triGetCurrentActiveServoAngle() * 10.0f;
        const int16_t servoSetpointAngle = getServoAngle(gpTailServoConf, toggleServoReverse(gpTailServoConf, *gpTailServo));

        const uint16_t maxPhaseShift = getPitchCorrectionMaxPhaseShift(servoAngle, servoSetpointAngle, tailMotorAccelerationDelay_angle, tailMotorDecelerationDelay_angle, tailMotorPitchZeroAngle);

        int16_t angleDiff = servoSetpointAngle - servoAngle;
        if (ABS(angleDiff) > maxPhaseShift)
        {
            angleDiff = (int32_t)maxPhaseShift * angleDiff / ABS(angleDiff);
        }

        const int16_t futureServoAngle = constrain(servoAngle + angleDiff, TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle, TRI_TAIL_SERVO_ANGLE_MID + tailServoMaxAngle);
        const uint16_t throttleMotorOutput = rcCommand[THROTTLE] - getCurrentMinthrottle();
        correction = (throttleMotorOutput * getPitchCorrectionAtTailAngle(DEGREES_TO_RADIANS(futureServoAngle / 10.0f), tailServoThrustFactor)) - throttleMotorOutput;
    }

    return correction;
}

_Bool triEnableServoUnarmed(void)
{
    return ((gpMixerConfig->tri_unarmed_servo != 0) || FLIGHT_MODE(TAILTUNE_MODE));
}

static uint16_t getServoValueAtAngle(servoParam_t *servoConf, uint16_t angle)
{
    const int16_t servoMid = servoConf->middle;
    uint16_t servoValue;

    if (angle < TRI_TAIL_SERVO_ANGLE_MID)
    {
        const int16_t servoMin = servoConf->min;
        servoValue = (int32_t)(angle - tailServoMaxAngle) * (servoMid - servoMin) / (TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle) + servoMin;
    }
    else if (angle > TRI_TAIL_SERVO_ANGLE_MID)
    {
        servoValue = (int32_t)(angle - TRI_TAIL_SERVO_ANGLE_MID) * (servoConf->max - servoMid) / tailServoMaxAngle + servoMid;
    }
    else
    {
        servoValue = servoMid;
    }

    return servoValue;
}

static float getPitchCorrectionAtTailAngle(float angle, float thrustFactor)
{
    return 1 / (sin_approx(angle) - cos_approx(angle) / thrustFactor);
}

static uint16_t getAngleFromYawCurveAtForce(int32_t force)
{
    if (force < yawForceCurve[0]) // No force that low
    {
        return TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE;
    }
    else if (!(force < yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE - 1])) // No force that high
    {
        return TRI_TAIL_SERVO_ANGLE_MID + TRI_TAIL_SERVO_MAX_ANGLE;
    }
    // Binary search: yawForceCurve[lower] <= force, yawForceCurve[higher] > force
    int32_t lower = 0, higher = TRI_YAW_FORCE_CURVE_SIZE - 1;
    while (higher > lower + 1)
    {
        const int32_t mid = (lower + higher) / 2;
        if (yawForceCurve[mid] > force)
        {
            higher = mid;
        }
        else
        {
            lower = mid;
        }
    }
    // Interpolating
    return TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE + lower * 10 + (int32_t)(force - yawForceCurve[lower]) * 10 / (yawForceCurve[higher] - yawForceCurve[lower]);
}

static uint16_t getServoAngle(servoParam_t *servoConf, uint16_t servoValue)
{
    const int16_t midValue = servoConf->middle;
    const int16_t endValue = servoValue < midValue ? servoConf->min : servoConf->max;
    const int16_t endAngle = servoValue < midValue ? TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle : TRI_TAIL_SERVO_ANGLE_MID + tailServoMaxAngle;
    const int16_t servoAngle = (int32_t)(endAngle - TRI_TAIL_SERVO_ANGLE_MID) * (servoValue - midValue) / (endValue - midValue) + TRI_TAIL_SERVO_ANGLE_MID;
    return servoAngle;
}

static uint16_t getPitchCorrectionMaxPhaseShift(int16_t servoAngle,
        int16_t servoSetpointAngle,
        int16_t motorAccelerationDelayAngle,
        int16_t motorDecelerationDelayAngle,
        int16_t motorDirectionChangeAngle)
{
    uint16_t maxPhaseShift;

    if (((servoAngle > servoSetpointAngle) && (servoAngle >= (motorDirectionChangeAngle + motorAccelerationDelayAngle))) ||
        ((servoAngle < servoSetpointAngle) && (servoAngle <= (motorDirectionChangeAngle - motorAccelerationDelayAngle))))
    {
        // Motor is braking
        maxPhaseShift = ABS(servoAngle - motorDirectionChangeAngle) >= motorDecelerationDelayAngle ?
                motorDecelerationDelayAngle:
                ABS(servoAngle - motorDirectionChangeAngle);
    }
    else
    {
        // Motor is accelerating
        maxPhaseShift = motorAccelerationDelayAngle;
    }

    return maxPhaseShift;
}

static void virtualServoStep(float dT, servoParam_t *servoConf, uint16_t servoValue)
{
    const float angleSetPoint = getServoAngle(servoConf, servoValue) / 10.0f;
    const float dA = dT * tailServoSpeed; // Max change of an angle since last check
    if ( fabsf(tailServoAngle.virtual - angleSetPoint) < dA )
    {
        // At set-point after this moment
        tailServoAngle.virtual = angleSetPoint;
    }
    else if (tailServoAngle.virtual < angleSetPoint)
    {
        tailServoAngle.virtual += dA;
    }
    else // tailServoAngle.virtual > angleSetPoint
    {
        tailServoAngle.virtual -= dA;
    }
}

static void triTailTuneStep(servoParam_t *pServoConf, int16_t *pServoVal)
{
    throttleStatus_e throttleStatus;
    if (!IS_RC_MODE_ACTIVE(BOXTAILTUNE))
    {
        if (FLIGHT_MODE(TAILTUNE_MODE))
        {
            DISABLE_ARMING_FLAG(PREVENT_ARMING);
            DISABLE_FLIGHT_MODE(TAILTUNE_MODE);
            tailTune.mode = TT_MODE_NONE;
        }
        return;
    }
    else
    {
        throttleStatus = calculateThrottleStatus(&masterConfig.rxConfig, masterConfig.flight3DConfig.deadband3d_throttle);
        ENABLE_FLIGHT_MODE(TAILTUNE_MODE);
    }

    if (tailTune.mode == TT_MODE_NONE)
    {
        if (ARMING_FLAG(ARMED))
        {
            tailTune.mode = TT_MODE_THRUST_TORQUE;
            tailTune.tt.state = TT_IDLE;
        }
        else
        {
            // Prevent accidental arming in servo setup mode
            ENABLE_ARMING_FLAG(PREVENT_ARMING);
            tailTune.mode = TT_MODE_SERVO_SETUP;
            tailTune.ss.servoVal = pServoConf->middle;
        }
    }

    switch (tailTune.mode)
    {
    case TT_MODE_THRUST_TORQUE:
        tailTuneModeThrustTorque(&tailTune.tt, throttleStatus);
        break;
    case TT_MODE_SERVO_SETUP:
        tailTuneModeServoSetup(&tailTune.ss, pServoConf, pServoVal);
        break;
    case TT_MODE_NONE:
        break;
    }
}

static void tailTuneModeThrustTorque(struct thrustTorque_t *pTT, throttleStatus_e throttleStatus)
{
    switch(pTT->state)
    {
    case TT_IDLE:
        // Calibration has been requested, only start when throttle is up
        if ((throttleStatus == THROTTLE_HIGH) && ARMING_FLAG(ARMED))
        {
            beeper(BEEPER_READY_BEEP);
            pTT->timestamp_ms = millis();
            pTT->lastAdjTime_ms = millis();
            pTT->state = TT_WAIT;
            pTT->servoAvgAngle.sum = 0;
            pTT->servoAvgAngle.numOf = 0;
        }
        break;
    case TT_WAIT:
        if ((throttleStatus == THROTTLE_HIGH) && ARMING_FLAG(ARMED))
        {
            /* Wait for 5 seconds before activating the tuning.
            This is so that pilot has time to take off if the tail tune mode was activated on ground. */
            if (IsDelayElapsed_ms(pTT->timestamp_ms, 5000))
            {
                pTT->state = TT_ACTIVE;
                pTT->timestamp_ms = millis();
            }
        }
        else
        {
            pTT->timestamp_ms = millis();
        }
        break;
    case TT_ACTIVE:
        if ((throttleStatus == THROTTLE_HIGH) &&
            isRcAxisWithinDeadband(ROLL) &&
            isRcAxisWithinDeadband(PITCH) &&
            isRcAxisWithinDeadband(YAW) &&
            (fabsf(gyroADC[FD_YAW] * gyro.scale) <= 4.0f))
        {
            if (IsDelayElapsed_ms(pTT->timestamp_ms, 250))
            {
                // RC commands have been within deadbands for 500ms
                if (IsDelayElapsed_ms(pTT->lastAdjTime_ms, 10))
                {
                    pTT->lastAdjTime_ms = millis();

                    pTT->servoAvgAngle.sum += triGetCurrentActiveServoAngle();
                    pTT->servoAvgAngle.numOf++;

                    beeperConfirmationBeeps(1);

                    if (pTT->servoAvgAngle.numOf >= 300)
                    {
                        beeper(BEEPER_READY_BEEP);
                        pTT->state = TT_WAIT_FOR_DISARM;
                        pTT->timestamp_ms = millis();
                    }
                }
            }
        }
        else
        {
            pTT->timestamp_ms = millis();
        }
        break;
    case TT_WAIT_FOR_DISARM:
        if (!ARMING_FLAG(ARMED))
        {
            float averageServoAngle = pTT->servoAvgAngle.sum / pTT->servoAvgAngle.numOf;

            // Find out the factor that gives least yaw force at the average angle
            float factor = TAIL_THRUST_FACTOR_MIN_FLOAT;
            const float angleRad = DEGREES_TO_RADIANS(averageServoAngle);
            float minAbsForce = FLT_MAX;
            float minFactor = TAIL_THRUST_FACTOR_MIN_FLOAT;
            _Bool done = false;
            for (factor = TAIL_THRUST_FACTOR_MIN_FLOAT; (done == false) && (factor < TAIL_THRUST_FACTOR_MAX_FLOAT); factor += 0.1f)
            {
                float absForceAtAngle = fabsf(-factor * cosf(angleRad) - sinf(angleRad)) * getPitchCorrectionAtTailAngle(angleRad, factor);

                if (absForceAtAngle < minAbsForce)
                {
                    minAbsForce = absForceAtAngle;
                    minFactor = factor;
                }
                else
                {
                    done = true;
                }
            }

            if (done)
            {
                gpMixerConfig->tri_tail_motor_thrustfactor = minFactor * 10.0f;

                saveConfigAndNotify();

                pTT->state = TT_DONE;
            }
            else
            {
                pTT->state = TT_FAIL;
            }
            pTT->timestamp_ms = millis();
        }
        else
        {
            if (IsDelayElapsed_ms(pTT->timestamp_ms, 2000))
            {
                beeper(BEEPER_READY_BEEP);
                pTT->timestamp_ms = millis();
            }
        }
        break;
    case TT_DONE:
        if (IsDelayElapsed_ms(pTT->timestamp_ms, 2000))
        {
            beeper(BEEPER_READY_BEEP);
            pTT->timestamp_ms = millis();
        }
        break;
    case TT_FAIL:
        if (IsDelayElapsed_ms(pTT->timestamp_ms, 2000))
        {
            beeper(BEEPER_ACC_CALIBRATION_FAIL);
            pTT->timestamp_ms = millis();
        }
        break;
    }
}

static void tailTuneModeServoSetup(struct servoSetup_t *pSS, servoParam_t *pServoConf, int16_t *pServoVal)
{
    // Check mode select
    if (isRcAxisWithinDeadband(PITCH) && (rcCommand[ROLL] < -100))
    {
        pSS->servoVal = pServoConf->min;
        pSS->pLimitToAdjust = &pServoConf->min;
        beeperConfirmationBeeps(1);
        pSS->state = SS_SETUP;
    }
    else if (isRcAxisWithinDeadband(ROLL) && (rcCommand[PITCH] > 100))
    {
        pSS->servoVal = pServoConf->middle;
        pSS->pLimitToAdjust = &pServoConf->middle;
        beeperConfirmationBeeps(2);
        pSS->state = SS_SETUP;
    }
    else if (isRcAxisWithinDeadband(PITCH) && (rcCommand[ROLL] > 100))
    {
        pSS->servoVal = pServoConf->max;
        pSS->pLimitToAdjust = &pServoConf->max;
        beeperConfirmationBeeps(3);
        pSS->state = SS_SETUP;
    }
    else if (isRcAxisWithinDeadband(ROLL) && (rcCommand[PITCH] < -100))
    {
        pSS->state = SS_CALIB;
        pSS->cal.state = SS_C_IDLE;
    }

    switch (pSS->state)
    {
    case SS_IDLE:
        break;
    case SS_SETUP:
        if (!isRcAxisWithinDeadband(YAW))
        {
            pSS->servoVal += -1.0f * (float)rcCommand[YAW] * dT;
            constrain(pSS->servoVal, 950, 2050);
            *pSS->pLimitToAdjust = pSS->servoVal;
        }
        break;
    case SS_CALIB:
        // State transition
        if ((pSS->cal.done == true) || (pSS->cal.state == SS_C_IDLE))
        {
            if (pSS->cal.state == SS_C_IDLE)
            {
                pSS->cal.state = SS_C_CALIB_MIN_MID_MAX;
                pSS->cal.subState = SS_C_MIN;
                pSS->servoVal = pServoConf->min;
                pSS->cal.avg.pCalibConfig = &gpMixerConfig->tri_servo_min_adc;
            }
            else if (pSS->cal.state == SS_C_CALIB_SPEED)
            {
                pSS->state = SS_IDLE;
                pSS->cal.subState = SS_C_IDLE;
                beeper(BEEPER_READY_BEEP);
            }
            else
            {
                if (pSS->cal.state == SS_C_CALIB_MIN_MID_MAX)
                {
                    switch (pSS->cal.subState)
                    {
                    case SS_C_MIN:
                        pSS->cal.subState = SS_C_MID;
                        pSS->servoVal = pServoConf->middle;
                        pSS->cal.avg.pCalibConfig = &gpMixerConfig->tri_servo_mid_adc;
                        break;
                    case SS_C_MID:
                        if (ABS(gpMixerConfig->tri_servo_min_adc - gpMixerConfig->tri_servo_mid_adc) < 100)
                        {
                            /* Not enough difference between min and mid feedback values.
                             * Most likely the feedback signal is not connected.
                             */
                            pSS->state = SS_IDLE;
                            pSS->cal.subState = SS_C_IDLE;
                            beeper(BEEPER_ACC_CALIBRATION_FAIL);
                            gpMixerConfig->tri_servo_feedback_mode = TRI_SERVO_VIRTUAL;
                        }
                        else
                        {
                            pSS->cal.subState = SS_C_MAX;
                            pSS->servoVal = pServoConf->max;
                            pSS->cal.avg.pCalibConfig = &gpMixerConfig->tri_servo_max_adc;
                            gpMixerConfig->tri_servo_feedback_mode = TRI_SERVO_FEEDBACK;
                        }
                        break;
                    case SS_C_MAX:
                        pSS->cal.state = SS_C_CALIB_SPEED;
                        pSS->cal.subState = SS_C_MIN;
                        pSS->servoVal = pServoConf->min;
                        pSS->cal.waitingServoToStop = true;
                        break;
                    }
                }
            }

            pSS->cal.timestamp_ms = millis();
            pSS->cal.avg.sum = 0;
            pSS->cal.avg.numOf = 0;
            pSS->cal.done = false;
        }

        switch (pSS->cal.state)
        {
        case SS_C_IDLE:
            break;
        case SS_C_CALIB_MIN_MID_MAX:
            if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 500))
            {
                if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 600))
                {
                    *pSS->cal.avg.pCalibConfig = pSS->cal.avg.sum / pSS->cal.avg.numOf;
                    pSS->cal.done = true;
                }
                else
                {
                    pSS->cal.avg.sum += tailServoADC;
                    pSS->cal.avg.numOf++;
                }
            }
            break;
        case SS_C_CALIB_SPEED:
            switch (pSS->cal.subState)
            {
            case SS_C_MIN:
                // Wait for the servo to reach min position
                if (tailServoADC < (gpMixerConfig->tri_servo_min_adc + 10))
                {
                    if (!pSS->cal.waitingServoToStop)
                    {
                        pSS->cal.avg.sum += millis() - pSS->cal.timestamp_ms;
                        pSS->cal.avg.numOf++;

                        if (pSS->cal.avg.numOf > 5)
                        {
                            const float avgTime = pSS->cal.avg.sum / pSS->cal.avg.numOf;
                            const float avgServoSpeed = (2.0f * tailServoMaxAngle / 10.0f) / avgTime * 1000.0f;
                            gpMixerConfig->tri_tail_servo_speed = avgServoSpeed;
                            tailServoSpeed = gpMixerConfig->tri_tail_servo_speed;
                            pSS->cal.done = true;
                            pSS->servoVal = pServoConf->middle;
                        }

                        pSS->cal.timestamp_ms = millis();
                        pSS->cal.waitingServoToStop = true;
                    }
                    // Wait for the servo to fully stop before starting speed measuring
                    else if  (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 200))
                    {
                        pSS->cal.timestamp_ms = millis();
                        pSS->cal.subState = SS_C_MAX;
                        pSS->cal.waitingServoToStop = false;
                        pSS->servoVal = pServoConf->max;
                    }
                }
                break;
            case SS_C_MAX:
                // Wait for the servo to reach max position
                if (tailServoADC > (gpMixerConfig->tri_servo_max_adc - 10))
                {
                    if (!pSS->cal.waitingServoToStop)
                    {
                        pSS->cal.avg.sum += millis() - pSS->cal.timestamp_ms;
                        pSS->cal.avg.numOf++;
                        pSS->cal.timestamp_ms = millis();
                        pSS->cal.waitingServoToStop = true;
                    }
                    else if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 200))
                    {
                        pSS->cal.timestamp_ms = millis();
                        pSS->cal.subState = SS_C_MIN;
                        pSS->cal.waitingServoToStop = false;
                        pSS->servoVal = pServoConf->min;
                    }
                }
                break;
            case SS_C_MID:
                // Should not come here
                break;
            }
        }
        break;
    }


    *pServoVal = pSS->servoVal;
}

static void updateServoAngles(void)
{
    // Virtual servo
    virtualServoStep(dT, gpTailServoConf, *gpTailServo);

    // Feedback servo
    if (gpMixerConfig->tri_servo_feedback_mode == TRI_SERVO_FEEDBACK)
    {
        const float ADCFeedback = tailServoADC;
        const float midValue = gpMixerConfig->tri_servo_mid_adc;
        const float endValue = ADCFeedback < midValue ? gpMixerConfig->tri_servo_min_adc : gpMixerConfig->tri_servo_max_adc;
        const float endAngle = ADCFeedback < gpMixerConfig->tri_servo_mid_adc ? TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle : TRI_TAIL_SERVO_ANGLE_MID + tailServoMaxAngle;
        tailServoAngle.feedback = 0.1f * ((endAngle - TRI_TAIL_SERVO_ANGLE_MID) * (ADCFeedback - midValue) / (endValue - midValue) + TRI_TAIL_SERVO_ANGLE_MID);
    }
}
