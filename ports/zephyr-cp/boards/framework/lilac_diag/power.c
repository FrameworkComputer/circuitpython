// Native AP power-rail control for the Lilac diagnostic target: pins, timing
// and locking around powerctl.c. A 1 ms kernel timer (ISR context, so it
// keeps running while Python is busy, interrupted, reloading or crashed)
// samples the power-good and sleep inputs, runs the protection and
// power-down logic and applies its writes. Python requests outputs through
// lilac.power_*; every assertion is refused while LILAC_POWER_ARMED is 0.
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lilac_diag.h"
#include "powerctl.h"
#include "keyboard.h"

LOG_MODULE_REGISTER(lilac_power, LOG_LEVEL_INF);

BUILD_ASSERT(LILAC_POWER_OUTPUTS == PWR_OUT_COUNT, "lilac_diag.h output count");
BUILD_ASSERT(LILAC_POWER_INPUTS == PWR_IN_COUNT, "lilac_diag.h input count");

struct pin { const struct device *port; gpio_pin_t pin; };
#define PIN(port, n) { DEVICE_DT_GET(DT_NODELABEL(port)), n }

// Same pins as the diagnostic output table (stock lilac/gpio.dtsi); level is
// physical, none of these is active low.
static const struct pin out_pins[PWR_OUT_COUNT] = {
    [PWR_SYS_PWRGD] = PIN(gpio7, 0),
    [PWR_VR_ON] = PIN(gpio6, 2),
    [PWR_SSD] = PIN(gpiob, 0),
    [PWR_SUSP] = PIN(gpio6, 3),
    [PWR_0P75VS] = PIN(gpioa, 2),
    [PWR_SYSON] = PIN(gpioa, 6),
    [PWR_RSMRST] = PIN(gpio5, 6),
    [PWR_PBTN] = PIN(gpio5, 0),
    [PWR_APU_AUD] = PIN(gpioa, 0),
    [PWR_PCH] = PIN(gpiod, 4),
    [PWR_0P75_1P8VALW] = PIN(gpiod, 7),
};

static const struct pin in_pins[PWR_IN_COUNT] = {
    [PWR_IN_ALW_PG] = PIN(gpio9, 4),   // pwr_3v5v_pg
    [PWR_IN_VR_PG] = PIN(gpio9, 7),    // apu_core_pwrgd
    [PWR_IN_SLP_S3] = PIN(gpioa, 3),   // slp_s3_l
    [PWR_IN_SLP_S5] = PIN(gpio9, 5),   // slp_s5_l
};

// Image 12: S0 peripheral outputs following powerctl_s0_peripherals(), as
// stock's chipset resume/suspend hooks. ec_bkoff_l (GPC7) is the panel
// backlight enable (stock gpio_sm_panel_bken_ec): high = backlight allowed.
// The diagnostic table configures it low at boot and on every reload.
static const struct pin bkoff_pin = PIN(gpioc, 7);
static bool periph_on;
// GPA7 powers the fingerprint/power-button LED, separately from PWM5/GPB7.
// Stock peripheral_power_startup/shutdown raises/lowers gpio_ec_fp_en.
static const struct pin fp_power_pin = PIN(gpioa, 7);
static bool fp_power_on;

static struct k_spinlock lock;
static struct powerctl pc;
static struct k_timer timer;
static uint32_t ticks, write_errors, input_errors;

static void apply(const struct powerctl_write *w, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (w[i].out == PWR_SYSON && !w[i].level) { lilac_keyboard_power(false); }
        const struct pin *p = &out_pins[w[i].out];
        if (gpio_pin_set_raw(p->port, p->pin, w[i].level) != 0) {
            write_errors++;
        }
    }
    // Runs after every write batch (also empty timer batches), so enable
    // follows an accepted SYSON request immediately and errors retry at 1 ms.
    // Explicit power-off and reload apply their off batch before returning.
    bool enabled = powerctl_startup_peripherals(&pc);
    if (enabled != fp_power_on) {
        if (gpio_pin_set_raw(fp_power_pin.port, fp_power_pin.pin, enabled) == 0) {
            fp_power_on = enabled;
        } else {
            write_errors++;
        }
    }
}

static void sample(int64_t now) {
    uint8_t in[PWR_IN_COUNT];
    for (int i = 0; i < PWR_IN_COUNT; i++) {
        int level = gpio_pin_get_raw(in_pins[i].port, in_pins[i].pin);
        if (level < 0) {
            input_errors++;
            level = 0;  // an unreadable power good counts as lost
        }
        in[i] = (uint8_t)level;
    }
    powerctl_inputs(&pc, in, now);
    pc.vw_ready = lilac_espi_vw_ready();
    struct lilac_fan_status fan;
    lilac_fan_status(&fan);
    pc.thermal_halt = fan.halt;
}

static void timer_handler(struct k_timer *t) {
    ARG_UNUSED(t);
    struct powerctl_write w[PWR_MAX_WRITES];
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&lock);
    enum powerctl_fault before = pc.fault;
    sample(now);
    size_t n = powerctl_tick(&pc, now, w);
    apply(w, n);
    lilac_keyboard_power(pc.armed && !pc.fault && pc.out[PWR_SYSON]);
    bool periph = powerctl_s0_peripherals(&pc);
    if (periph != periph_on) {
        if (gpio_pin_set_raw(bkoff_pin.port, bkoff_pin.pin, periph) == 0) {
            periph_on = periph;  // on error: retried next tick
        } else {
            write_errors++;
        }
    }
    ticks++;
    enum powerctl_fault after = pc.fault;
    k_spin_unlock(&lock, key);
    if (after != before && after != PWR_FAULT_NONE) {
        LOG_ERR("power fault %s: all rails off", powerctl_fault_name(after));
    }
}

static int lilac_power_init(void) {
    powerctl_init(&pc, LILAC_POWER_ARMED, k_uptime_get());
    k_timer_init(&timer, timer_handler, NULL);
    k_timer_start(&timer, K_MSEC(1), K_MSEC(1));
    return 0;
}
SYS_INIT(lilac_power_init, APPLICATION, 1);

int lilac_power_set(const char *name, int level) {
    int out = powerctl_out_index(name);
    if (out < 0) {
        return -EINVAL;
    }
    struct powerctl_write w[PWR_MAX_WRITES];
    size_t n;
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&lock);
    sample(now);
    int ret = powerctl_request(&pc, out, level, now, w, &n);
    apply(w, n);
    k_spin_unlock(&lock, key);
    return ret;
}

int lilac_power_pbtn_pulse(void) {
    struct powerctl_write w[PWR_MAX_WRITES];
    size_t n;
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&lock);
    int ret = powerctl_pbtn_pulse(&pc, now, w, &n);
    apply(w, n);
    k_spin_unlock(&lock, key);
    return ret;
}

void lilac_power_heartbeat(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    powerctl_heartbeat(&pc, k_uptime_get());
    k_spin_unlock(&lock, key);
}

void lilac_power_off(void) {
    struct powerctl_write w[PWR_MAX_WRITES];
    k_spinlock_key_t key = k_spin_lock(&lock);
    size_t n = powerctl_power_off(&pc, k_uptime_get(), w);
    apply(w, n);
    k_spin_unlock(&lock, key);
}

// Called before the diagnostic pin table is reapplied on a Python reload:
// rails still up are switched off in force_g3 order first, latched.
void lilac_power_reload(void) {
    struct powerctl_write w[PWR_MAX_WRITES];
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (powerctl_any_on(&pc)) {
        size_t n = powerctl_emergency(&pc, PWR_FAULT_RELOAD, k_uptime_get(), w);
        apply(w, n);
    }
    k_spin_unlock(&lock, key);
}

int lilac_power_clear_fault(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    int ret = powerctl_clear_fault(&pc);
    k_spin_unlock(&lock, key);
    return ret;
}

void lilac_power_status(struct lilac_power_status *st) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    int64_t now = k_uptime_get();
    st->armed = pc.armed;
    st->fault = powerctl_fault_name(pc.fault);
    st->faults = pc.faults;
    st->fault_ms = pc.fault ? pc.fault_ms : -1;
    for (int i = 0; i < PWR_OUT_COUNT; i++) {
        st->out[i] = pc.out[i];
    }
    for (int i = 0; i < PWR_IN_COUNT; i++) {
        st->in[i] = pc.in[i];
    }
    st->vw_ready = pc.vw_ready;
    st->backlight = periph_on;
    st->fp_power = fp_power_on;
    st->lease_ms = powerctl_any_on(&pc) ? (int32_t)(pc.lease_until - now) : -1;
    st->ticks = ticks;
    st->write_errors = write_errors;
    st->input_errors = input_errors;
    k_spin_unlock(&lock, key);
}

const char *lilac_power_out_name(int out) {
    return powerctl_out_name(out);
}
