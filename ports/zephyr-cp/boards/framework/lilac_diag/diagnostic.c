// Lilac AP-off diagnostic candidate. Not electrically validated.
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/gpio/nuvoton-npcx-gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

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
    { DEVICE_DT_GET(DT_NODELABEL(gpio4)), 0, GPIO_OUTPUT_LOW }, // left_side
    { DEVICE_DT_GET(DT_NODELABEL(gpiod)), 3, GPIO_OUTPUT_LOW }, // right_side
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


// Read-only observations; these pads are inputs from reset and are not reconfigured.
struct lilac_input { const struct device *port; gpio_pin_t pin; const char *name; };
static const struct lilac_input inputs[] = {
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 4, "pg_3v5v" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 7, "pg_vr" },
    { DEVICE_DT_GET(DT_NODELABEL(gpioa)), 3, "slp_s3_l" },
    { DEVICE_DT_GET(DT_NODELABEL(gpio9)), 5, "slp_s5_l" },
};

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
    for (size_t i = 0; i < ARRAY_SIZE(inputs); i++) {
        printk(" %s=%d", inputs[i].name, gpio_pin_get_raw(inputs[i].port, inputs[i].pin));
    }
    printk("\n");
}

void lilac_diagnostic_reset(void) {
    // Attempt every pin even after an error, so one failure does not leave
    // the remaining controls unconfigured; then halt before Python.
    static bool reported;
    bool failed = false;
    for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
        if (!device_is_ready(pins[i].port) ||
            gpio_pin_configure(pins[i].port, pins[i].pin, pins[i].flags) != 0) {
            failed = true;
        }
    }
    if (failed) {
        printk("Lilac GPIO initialization failed; halting before Python\n");
        for (;;) { k_sleep(K_FOREVER); }
    }
    if (!reported) {
        reported = true;
        lilac_diagnostic_report();
    }
}
