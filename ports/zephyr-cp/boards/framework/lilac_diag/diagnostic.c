// Lilac AP-off diagnostic candidate. Not electrically validated.
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/dt-bindings/gpio/nuvoton-npcx-gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "lilac_diag.h"

LOG_MODULE_REGISTER(lilac_diag, LOG_LEVEL_INF);

struct lilac_pin { const struct device *port; gpio_pin_t pin; gpio_flags_t flags; };
static const struct lilac_pin pins[] = {
    { DEVICE_DT_GET(DT_NODELABEL(gpio5)), 6, (GPIO_OUTPUT_LOW | NPCX_GPIO_VOLTAGE_1P8) }, // ec_soc_rsmrst_l
    { DEVICE_DT_GET(DT_NODELABEL(gpioc)), 6, GPIO_OUTPUT_LOW }, // rtc_dis
    { DEVICE_DT_GET(DT_NODELABEL(gpio7)), 6, (GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN) }, // ec_sci_l
    { DEVICE_DT_GET(DT_NODELABEL(gpioc)), 5, GPIO_OUTPUT_LOW }, // otg_en
    { DEVICE_DT_GET(DT_NODELABEL(gpiod)), 7, GPIO_OUTPUT_LOW }, // 0p75_1p8valw_pwren
    { DEVICE_DT_GET(DT_NODELABEL(gpio8)), 6, GPIO_OUTPUT_LOW }, // module_pwr_on
    { DEVICE_DT_GET(DT_NODELABEL(gpiod)), 4, GPIO_OUTPUT_LOW }, // pch_pwr_en
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 1, (GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN) }, // h_prochot_l
    { DEVICE_DT_GET(DT_NODELABEL(gpio6)), 7, GPIO_OUTPUT_LOW }, // cam_en
    { DEVICE_DT_GET(DT_NODELABEL(gpio7)), 0, GPIO_OUTPUT_LOW }, // sys_pwrgd_ec
    { DEVICE_DT_GET(DT_NODELABEL(gpio6)), 2, GPIO_OUTPUT_LOW }, // vr_on
    { DEVICE_DT_GET(DT_NODELABEL(gpio6)), 3, GPIO_OUTPUT_LOW }, // susp_l
    { DEVICE_DT_GET(DT_NODELABEL(gpio4)), 0, GPIO_OUTPUT_LOW }, // left_side: battery LED select
    { DEVICE_DT_GET(DT_NODELABEL(gpiod)), 3, GPIO_OUTPUT_LOW }, // right_side: battery LED select
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 6, GPIO_OUTPUT_LOW }, // syson
    { DEVICE_DT_GET(DT_NODELABEL(gpio0)), 3, GPIO_OUTPUT_LOW }, // tp_board_id_pug
    { DEVICE_DT_GET(DT_NODELABEL(gpiob)), 1, GPIO_OUTPUT_LOW }, // cap_led
    { DEVICE_DT_GET(DT_NODELABEL(gpio4)), 2, GPIO_OUTPUT_LOW }, // ec_beep
    { DEVICE_DT_GET(DT_NODELABEL(gpioc)), 7, GPIO_OUTPUT_LOW }, // ec_bkoff_l
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 2, GPIO_OUTPUT_LOW }, // 0p75vs_pwr_en
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 0, GPIO_OUTPUT_LOW }, // apu_aud_pwr_en
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 6, (GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN) }, // apu_ec_int_l
    { DEVICE_DT_GET(DT_NODELABEL(gpio7)), 4, (GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN) }, // apu_ec_int2_l
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 7, GPIO_OUTPUT_LOW }, // ec_fp_en
    { DEVICE_DT_GET(DT_NODELABEL(gpiob)), 0, GPIO_OUTPUT_LOW }, // ssd_pwr_en
    { DEVICE_DT_GET(DT_NODELABEL(gpio6)), 1, GPIO_OUTPUT_LOW }, // wl_rst_l
    { DEVICE_DT_GET(DT_NODELABEL(gpio7)), 2, GPIO_OUTPUT_LOW }, // wlan_en
    { DEVICE_DT_GET(DT_NODELABEL(gpio5)), 0, ((GPIO_OUTPUT_LOW | GPIO_OPEN_DRAIN) | NPCX_GPIO_VOLTAGE_1P8) }, // pbtn_out_l
};


// Named inputs, configured with their stock flags (input, no pulls; 1.8 V
// detection for eSPI reset) and readable from Python. Pinmux is untouched.
// The first four are printed in the boot report.
struct lilac_input { const struct device *port; gpio_pin_t pin; gpio_flags_t flags; const char *name; };
static const struct lilac_input inputs[] = {
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 4, GPIO_INPUT, "pg_3v5v" }, // pwr_3v5v_pg
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 7, GPIO_INPUT, "pg_vr" }, // apu_core_pwrgd
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 3, GPIO_INPUT, "slp_s3_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 5, GPIO_INPUT, "slp_s5_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio8)), 1, GPIO_INPUT, "thermtrip" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio5)), 4, (GPIO_INPUT | NPCX_GPIO_VOLTAGE_1P8), "ec_espi_rst_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpiod)), 2, GPIO_INPUT, "on_off_btn_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio0)), 0, GPIO_INPUT, "chassis_open_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio0)), 1, GPIO_INPUT, "hw_acav_in" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio0)), 2, GPIO_INPUT, "lid_sw_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio4)), 3, GPIO_INPUT, "ac_present_pd_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpioc)), 1, GPIO_INPUT, "mic_sw" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 3, GPIO_INPUT, "cam_sw" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio3)), 4, GPIO_INPUT, "fp_control" },
    { DEVICE_DT_GET(DT_NODELABEL(gpioe)), 0, GPIO_INPUT, "apu_enbkl_r" },
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 5, GPIO_INPUT, "ec_mute_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpiod)), 5, GPIO_INPUT, "ddr5_sodimm_pwrgd_r" },
    { DEVICE_DT_GET(DT_NODELABEL(gpioe)), 5, GPIO_INPUT, "pd_a_i2c_int_ec_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpioe)), 2, GPIO_INPUT, "pd_b_i2c_int_ec_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 4, GPIO_INPUT, "i2c_int_tp_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio7)), 5, GPIO_INPUT, "als_int_l" },
};
#define LILAC_REPORTED_INPUTS 4
// slp_s3_l: stock power-good pin of the F75303 sensors and the fan.
#define LILAC_INPUT_SLP_S3_L 2

// One-time report after the first setup: pad levels as the EC reads them, in
// table order. A pad driven low that reads high indicates contention.

static void lilac_diagnostic_report(void) {
    char levels[ARRAY_SIZE(pins) + 1];
    unsigned stuck = 0;
    k_msleep(1);
    for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
        int level = gpio_pin_get_raw(pins[i].port, pins[i].pin);
        levels[i] = level < 0 ? '?' : (char)('0' + level);
        if (!(pins[i].flags & GPIO_OUTPUT_INIT_HIGH) && level != 0) {
            stuck++;
        }
    }
    levels[ARRAY_SIZE(pins)] = '\0';
    printk("Lilac diag: %u/%u pins configured, pads=%s, low-driven-not-low=%u\n",
        (unsigned)ARRAY_SIZE(pins), (unsigned)ARRAY_SIZE(pins), levels, stuck);
    printk("Lilac diag: inputs");
    for (size_t i = 0; i < LILAC_REPORTED_INPUTS; i++) {
        printk(" %s=%d", inputs[i].name, gpio_pin_get_raw(inputs[i].port, inputs[i].pin));
    }
    printk("\n");
}

// Python chooses brightness; native S0 gating survives Python failure.
static const struct pwm_dt_spec led_power = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_power));
static struct k_work_delayable power_led_work;
static atomic_t power_led_percent = ATOMIC_INIT(55);

int lilac_power_led_set(int percent) {
    if (percent < 0 || percent > 100) {
        return -EINVAL;
    }
    if (!pwm_is_ready_dt(&led_power)) {
        return -ENODEV;
    }
    atomic_set(&power_led_percent, percent);
    return 0;
}

static void power_led_update(struct k_work *work) {
    struct lilac_power_status st;
    lilac_power_status(&st);
    // Same native S0 condition as the panel: SYS_PWRGD and SLP_S3#.
    uint32_t percent = st.backlight ? atomic_get(&power_led_percent) : 0;
    int ret = pwm_set_pulse_dt(&led_power, led_power.period * percent / 100);
    if (ret != 0) {
        LOG_ERR("power LED PWM write failed: %d", ret);
    }
    k_work_schedule(&power_led_work, K_MSEC(20));
}


size_t lilac_input_count(void) {
    return ARRAY_SIZE(inputs);
}

const char *lilac_input_name(size_t index) {
    return index < ARRAY_SIZE(inputs) ? inputs[index].name : NULL;
}

int lilac_input_get(size_t index) {
    if (index >= ARRAY_SIZE(inputs)) {
        return -EINVAL;
    }
    return gpio_pin_get_raw(inputs[index].port, inputs[index].pin);
}

// Register reads only, at fixed addresses, limited to the registers below.
// ISL9241 (I2C3, 0x09): SMBus read word, low byte first. Limits, controls,
// information, ADC results and IDs as named in ec-lilac isl9241.h.
static const uint8_t charger_regs[] = {
    0x14, 0x15, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x43,
    0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x90, 0x91, 0xfe, 0xff,
};
// F75303 (I2C7, 0x4D): temperature high/low bytes for local, remote 1 and
// remote 2, the only registers stock reads.
static const uint8_t thermal_regs[] = { 0x00, 0x29, 0x01, 0x10, 0x23, 0x24 };

static const struct i2c_dt_spec charger = I2C_DT_SPEC_GET(DT_NODELABEL(lilac_charger));
static const struct i2c_dt_spec thermal = I2C_DT_SPEC_GET(DT_NODELABEL(lilac_thermal));

static bool reg_allowed(const uint8_t *list, size_t count, uint8_t reg) {
    for (size_t i = 0; i < count; i++) {
        if (list[i] == reg) {
            return true;
        }
    }
    return false;
}

const uint8_t *lilac_charger_registers(size_t *count) {
    *count = ARRAY_SIZE(charger_regs);
    return charger_regs;
}

const uint8_t *lilac_thermal_registers(size_t *count) {
    *count = ARRAY_SIZE(thermal_regs);
    return thermal_regs;
}

int lilac_charger_read(uint8_t reg, uint16_t *value) {
    uint8_t buf[2];
    if (!reg_allowed(charger_regs, ARRAY_SIZE(charger_regs), reg)) {
        return -EPERM;
    }
    if (!i2c_is_ready_dt(&charger)) {
        return -ENODEV;
    }
    int ret = i2c_write_read_dt(&charger, &reg, 1, buf, sizeof(buf));
    if (ret != 0) {
        LOG_WRN("charger read 0x%02x failed: %d", reg, ret);
        return ret;
    }
    *value = (uint16_t)(buf[0] | (buf[1] << 8));
    return 0;
}

// The only charger write path: read-modify-write of allowlisted bits, as the
// stock driver's isl9241_update(), with the masked value bounded natively.
// CONTROL3 bit 0 keeps the ADC running when not charging (stock sets it
// around each VIN/VSYS read). Image 9: the adapter (input) current limits,
// which stock writes together in isl9241_set_input_current_limit(); with the
// stock 20 mOhm input sense resistor the register value is mA (bits 12:2).
// Bring-up ceiling 3000 mA (user decision 2026-11-15; stock sets ~4748 mA
// from the PD contract), floor 500 mA (stock default input limit).
static const struct lilac_writable charger_writable[] = {
    { 0x4c, 0x0001, 0, 0x0001 },      // CONTROL3 ENABLE_ADC
    { 0x3f, 0x1ffc, 500, 3000 },      // ADAPTER_CUR_LIMIT1, mA
    { 0x3b, 0x1ffc, 500, 3000 },      // ADAPTER_CUR_LIMIT2, mA
};

const struct lilac_writable *lilac_charger_writable(size_t *count) {
    *count = ARRAY_SIZE(charger_writable);
    return charger_writable;
}

int lilac_charger_update(uint8_t reg, uint16_t mask, uint16_t value, uint16_t *result) {
    const struct lilac_writable *w = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(charger_writable); i++) {
        if (charger_writable[i].reg == reg && mask != 0 &&
            (mask & ~charger_writable[i].mask) == 0) {
            w = &charger_writable[i];
        }
    }
    if (w == NULL) {
        return -EPERM;
    }
    if (value & ~mask) {
        return -EINVAL;
    }
    // Bounds apply to the whole field: a partial mask could otherwise build
    // an out-of-range value from the bits it leaves alone.
    bool ranged = !(w->min == 0 && w->max == w->mask);
    if (ranged && (mask != w->mask || value < w->min || value > w->max)) {
        return -ERANGE;
    }
    uint16_t current;
    int ret = lilac_charger_read(reg, &current);
    if (ret != 0) {
        return ret;
    }
    uint16_t next = (uint16_t)((current & ~mask) | value);
    if (next != current) {
        uint8_t buf[3] = { reg, (uint8_t)(next & 0xff), (uint8_t)(next >> 8) };
        ret = i2c_write_dt(&charger, buf, sizeof(buf));
        if (ret != 0) {
            LOG_WRN("charger write 0x%02x failed: %d", reg, ret);
            return ret;
        }
        LOG_INF("charger 0x%02x: 0x%04x -> 0x%04x", reg, current, next);
    }
    ret = lilac_charger_read(reg, &current);
    if (ret != 0) {
        return ret;
    }
    *result = current;
    if ((current & mask) != value) {
        LOG_WRN("charger 0x%02x readback 0x%04x, expected bits 0x%04x", reg, current, value);
        return -EIO;
    }
    return 0;
}

int lilac_sensor_power(void) {
    return lilac_input_get(LILAC_INPUT_SLP_S3_L);
}

// The F75303 sits on a rail that is up only in S0, as the stock sensors'
// power-good pin (slp_s3_l) says; stock reads none of them otherwise. With
// that pin low the bus is not touched (in G3 it is held low and the driver's
// recovery attempt fails), and the read returns -EAGAIN.
static int thermal_read(uint8_t reg, uint8_t *value, bool quiet) {
    if (!reg_allowed(thermal_regs, ARRAY_SIZE(thermal_regs), reg)) {
        return -EPERM;
    }
    int power = lilac_sensor_power();
    if (power < 0) {
        return power;
    }
    if (power != 1) {
        return -EAGAIN;
    }
    if (!i2c_is_ready_dt(&thermal)) {
        return -ENODEV;
    }
    int ret = i2c_write_read_dt(&thermal, &reg, 1, value, 1);
    if (ret != 0 && !quiet) {
        LOG_WRN("thermal read 0x%02x failed: %d", reg, ret);
    }
    return ret;
}

int lilac_thermal_read(uint8_t reg, uint8_t *value) {
    return thermal_read(reg, value, false);
}

int lilac_thermal_read_quiet(uint8_t reg, uint8_t *value) {
    return thermal_read(reg, value, true);
}

void lilac_diagnostic_reset(void) {
    // Attempt every pin even after an error, so one failure does not leave
    // the remaining controls unconfigured; then halt before Python.
    static bool reported;
    bool failed = false;
    // Rails still up after a Python reload go off in stock order, latched,
    // before the table below drives every output low anyway.
    lilac_power_reload();
    atomic_set(&power_led_percent, 55);
    if (pwm_is_ready_dt(&led_power)) {
        pwm_set_pulse_dt(&led_power, 0);
    }
    for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
        if (!device_is_ready(pins[i].port) ||
            gpio_pin_configure(pins[i].port, pins[i].pin, pins[i].flags) != 0) {
            failed = true;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(inputs); i++) {
        if (!device_is_ready(inputs[i].port) ||
            gpio_pin_configure(inputs[i].port, inputs[i].pin, inputs[i].flags) != 0) {
            failed = true;
        }
    }
    if (failed) {
        printk("Lilac GPIO initialization failed; halting before Python\n");
        for (;;) { k_sleep(K_FOREVER); }
    }
    // Blank RGB on every reset/reload; policy starts in Python.
    if (lilac_side_led_set(0, 0, 0, 0) != 0) {
        LOG_WRN("side LED initialization failed");
    }
    if (!reported) {
        reported = true;
        lilac_diagnostic_report();

        lilac_fan_start();
        k_work_init_delayable(&power_led_work, power_led_update);
        k_work_schedule(&power_led_work, K_NO_WAIT);
    }
}
