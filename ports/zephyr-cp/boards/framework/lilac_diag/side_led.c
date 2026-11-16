// Lilac side RGB LEDs: mechanism only. Python owns the stock LED policy.
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include "lilac_diag.h"

static const struct pwm_dt_spec channels[] = {
    PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_red)),
    PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_green)),
    PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_blue)),
};
static const struct gpio_dt_spec left = { DEVICE_DT_GET(DT_NODELABEL(gpio4)), 0, 0 };
static const struct gpio_dt_spec right = { DEVICE_DT_GET(DT_NODELABEL(gpiod)), 3, 0 };

// Blank the selects while changing shared RGB PWM, to avoid a wrong-color
// flash on the newly selected side. Leave both blank on any hardware error.
int lilac_side_led_set(int red, int green, int blue, int sides) {
    int duty[] = { red, green, blue };
    if (sides < 0 || sides > 3) {
        return -EINVAL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
        if (duty[i] < 0 || duty[i] > 100) {
            return -EINVAL;
        }
        if (!pwm_is_ready_dt(&channels[i])) {
            return -ENODEV;
        }
    }
    int ret = gpio_pin_set_dt(&left, 0);
    int other = gpio_pin_set_dt(&right, 0);
    if (ret || other) {
        return ret ? ret : other;
    }
    for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
        ret = pwm_set_pulse_dt(&channels[i], channels[i].period * duty[i] / 100);
        if (ret) {
            return ret;
        }
    }
    ret = gpio_pin_set_dt(&left, sides & 1);
    if (!ret) {
        ret = gpio_pin_set_dt(&right, (sides >> 1) & 1);
    }
    if (ret) {
        gpio_pin_set_dt(&left, 0);
        gpio_pin_set_dt(&right, 0);
    }
    return ret;
}
