// Lilac fan and native thermal floor, AP-off diagnostic target.
//
// Mechanism only (native/Python rule): Python decides the fan duty from its
// curve; native code owns the PWM and tach and overrides Python with 100 %
// when Python stops refreshing its request (crash, Ctrl-C, reload), when a
// sensor read fails, or when a sensor reaches its stock full-fan temperature.
// The fan and sensors are powered only in S0 (stock power-good pin
// slp_s3_l); with it low the duty is 0, as stock disables the fan, and no
// sensor is read. Runs on the system work queue, independent of Python.
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "lilac_diag.h"

LOG_MODULE_REGISTER(lilac_fan, LOG_LEVEL_INF);

// Stock ec-lilac lilac/overlay.dtsi (thermal table V1B_241118) and i2c.dtsi:
// local = F75303 local (Uth1), cpu = remote 2 (QN2), ddr = remote 1 (QN1).
// The APU's own SB-TSI sensor (0x4c) is not read by this image.
static const struct lilac_temp_limit limits[LILAC_TEMP_SENSORS] = {
    { "local", 0x00, 70, 120, 115 },
    { "cpu", 0x23, 69, 120, 115 },
    { "ddr", 0x01, 60, 120, 115 },
};

// As stock laptop13/fan.dtsi: PWM0 channel 0, 25 kHz, normal polarity; the
// pad is open-drain through pinctrl.
static const struct pwm_dt_spec fan_pwm = {
    .dev = DEVICE_DT_GET(DT_NODELABEL(pwm0)),
    .channel = 0,
    .period = PWM_KHZ(25),
    .flags = PWM_POLARITY_NORMAL,
};
static const struct device *const fan_tach = DEVICE_DT_GET(DT_NODELABEL(tach2));

static struct k_work_delayable fan_work;
static struct k_spinlock fan_lock;
static struct lilac_fan_status state;
static int64_t request_ms;
static bool pwm_applied;

const struct lilac_temp_limit *lilac_temp_limits(size_t *count) {
    *count = ARRAY_SIZE(limits);
    return limits;
}

static const char *const reason_names[] = { "unpowered", "python", "lease", "sensor", "hot" };

static void fan_update(struct k_work *work) {
    int16_t temp[LILAC_TEMP_SENSORS];
    bool powered = lilac_sensor_power() == 1;
    bool failed = false, hot = false, halt_reached = false, below_release = true;

    for (size_t i = 0; i < ARRAY_SIZE(limits); i++) {
        uint8_t raw;
        temp[i] = LILAC_TEMP_INVALID;
        if (!powered) {
            continue;
        }
        if (lilac_thermal_read_quiet(limits[i].reg, &raw) != 0) {
            failed = true;
            continue;
        }
        temp[i] = raw;
        hot |= temp[i] >= limits[i].full_c;
        halt_reached |= temp[i] >= limits[i].halt_c;
        below_release &= temp[i] < limits[i].release_c;
    }

    int32_t rpm = -ENODEV;
    if (device_is_ready(fan_tach)) {
        struct sensor_value value;
        rpm = sensor_sample_fetch_chan(fan_tach, SENSOR_CHAN_RPM);
        if (rpm == 0) {
            rpm = sensor_channel_get(fan_tach, SENSOR_CHAN_RPM, &value);
            if (rpm == 0) {
                rpm = value.val1;
            }
        }
    }

    k_spinlock_key_t key = k_spin_lock(&fan_lock);
    int64_t now = k_uptime_get();
    enum lilac_fan_reason reason;
    if (!powered) {
        reason = LILAC_FAN_UNPOWERED;
    } else if (failed) {
        reason = LILAC_FAN_SENSOR;
    } else if (hot) {
        reason = LILAC_FAN_HOT;
    } else if (!state.requested || now - request_ms >= LILAC_FAN_LEASE_MS) {
        reason = LILAC_FAN_LEASE;
    } else {
        reason = LILAC_FAN_PYTHON;
    }
    uint8_t duty = reason == LILAC_FAN_UNPOWERED ? 0
        : reason == LILAC_FAN_PYTHON ? state.request : 100;
    enum lilac_fan_reason previous = state.reason;
    bool was_halt = state.halt;
    bool changed = !pwm_applied || duty != state.duty;
    if (halt_reached) {
        state.halt = true;
    } else if (state.halt && powered && !failed && below_release) {
        state.halt = false;
    }
    state.powered = powered;
    state.reason = reason;
    state.duty = duty;
    state.rpm = rpm;
    state.updated_ms = now;
    for (size_t i = 0; i < ARRAY_SIZE(limits); i++) {
        state.temp_c[i] = temp[i];
    }
    k_spin_unlock(&fan_lock, key);

    if (changed) {
        int ret = pwm_is_ready_dt(&fan_pwm)
            ? pwm_set_pulse_dt(&fan_pwm, fan_pwm.period / 100 * duty) : -ENODEV;
        key = k_spin_lock(&fan_lock);
        state.pwm_error = ret;
        pwm_applied = ret == 0;
        k_spin_unlock(&fan_lock, key);
        if (ret != 0) {
            LOG_ERR("fan PWM write failed: %d", ret);
        }
    }
    if (reason != previous) {
        LOG_INF("fan %s, duty %u%%", reason_names[reason], duty);
    }
    if (state.halt != was_halt) {
        // There is no AP power path in this image, so nothing to shut down.
        LOG_ERR("thermal halt %s", state.halt ? "reached: shutdown required" : "released");
    }
    k_work_schedule(&fan_work, K_MSEC(LILAC_FAN_PERIOD_MS));
}

int lilac_fan_set(int percent) {
    if (percent < 0 || percent > 100) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&fan_lock);
    state.request = (uint8_t)percent;
    state.requested = true;
    request_ms = k_uptime_get();
    k_spin_unlock(&fan_lock, key);
    k_work_reschedule(&fan_work, K_NO_WAIT);
    return 0;
}

void lilac_fan_status(struct lilac_fan_status *status) {
    k_spinlock_key_t key = k_spin_lock(&fan_lock);
    *status = state;
    k_spin_unlock(&fan_lock, key);
}

void lilac_fan_start(void) {
    state.reason = LILAC_FAN_UNPOWERED;
    state.pwm_error = pwm_is_ready_dt(&fan_pwm) ? 0 : -ENODEV;
    for (size_t i = 0; i < ARRAY_SIZE(limits); i++) {
        state.temp_c[i] = LILAC_TEMP_INVALID;
    }
    printk("Lilac diag: fan pwm=%s tach=%s\n", state.pwm_error == 0 ? "ok" : "missing",
        device_is_ready(fan_tach) ? "ok" : "missing");
    k_work_init_delayable(&fan_work, fan_update);
    // The first pass applies duty 0 (G3) or the full-speed default (S0).
    k_work_schedule(&fan_work, K_NO_WAIT);
}
