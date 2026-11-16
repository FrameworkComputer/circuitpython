// Native keyboard path. Matrix timing/map and deck detection from stock
// ec-lilac laptop13/keyboard_customization_13.c and src/input_module_13.c.
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/espi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <soc.h>
#include "kbc.h"
#include "keyboard.h"
#include "lilac_diag.h"

static const struct device *const module_port = DEVICE_DT_GET(DT_NODELABEL(gpio8));
static const struct device *const pug_port = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const caps_port = DEVICE_DT_GET(DT_NODELABEL(gpiob));
static struct kbc controller;
static struct k_spinlock lock, deck_lock;
static bool ready, deck_allowed, deck_on;
static int deck_state, deck_mv = -1, deck_id = -1;
static uint32_t scan_events;
static uint8_t col, row;
static uint16_t scancodes[16][8] = {
		{0x0021, 0x007B, 0x0079, 0x0072, 0x007A, 0x0071, 0x0069, 0xe04A},
		{0xe071, 0xe070, 0x007D, 0xe01f, 0x006c, 0xe06c, 0xe07d, 0x0077},
		{0x0015, 0x0070, 0x00ff, 0x000D, 0x000E, 0x0016, 0x0067, 0x001c},
		{0xe011, 0x0011, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
		{0xe05a, 0x0029, 0x0024, 0x000c, 0x0058, 0x0026, 0x0004, 0xe07a},
		{0x0022, 0x001a, 0x0006, 0x0005, 0x001b, 0x001e, 0x001d, 0x0076},
		{0x002A, 0x0032, 0x0034, 0x002c, 0x002e, 0x0025, 0x002d, 0x002b},
		{0x003a, 0x0031, 0x0033, 0x0035, 0x0036, 0x003d, 0x003c, 0x003b},
		{0x0049, 0xe072, 0x005d, 0x0044, 0x0009, 0x0046, 0x0078, 0x004b},
		{0x0059, 0x0012, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
		{0x0041, 0x007c, 0x0083, 0x000b, 0x0003, 0x003e, 0x0043, 0x0042},
		{0x0013, 0x0064, 0x0075, 0x0001, 0x0051, 0x0061, 0xe06b, 0xe02f},
		{0xe014, 0x0014, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
		{0x004a, 0xe075, 0x004e, 0x0007, 0x0045, 0x004d, 0x0054, 0x004c},
		{0x0052, 0x005a, 0xe03c, 0xe069, 0x0055, 0x0066, 0x005b, 0x0023},
		{0x006a, 0x000a, 0xe074, 0xe054, 0x0000, 0x006b, 0x0073, 0x0074},
};
static void flush(void) {
    struct kbc_reg *hw = (void *)DT_REG_ADDR_BY_NAME(DT_NODELABEL(host_sub), kbc);
    if (!ready || (hw->HIKMST & BIT(NPCX_HIKMST_OBF))) { return; }
    uint16_t v;
    if (!kbc_pop(&controller, &v)) { return; }
    // The driver's hardware IRQ1/IRQ12 enables follow the controller RAM.
    // Use raw registers because the public driver rejects requests if either
    // IRQ is disabled. OBE is always enabled to drain multi-byte replies.
    if (v & 0x100) { hw->HIMDO = v; } else { hw->HIKDO = v; }
    hw->HICTRL |= BIT(NPCX_HICTRL_OBECIE);
}
static void flags(void) {
    struct kbc_reg *hw = (void *)DT_REG_ADDR_BY_NAME(DT_NODELABEL(host_sub), kbc);
    uint8_t ctl = hw->HICTRL & ~(BIT(NPCX_HICTRL_OBFKIE) | BIT(NPCX_HICTRL_OBFMIE));
    if (controller.ram[0] & 1) { ctl |= BIT(NPCX_HICTRL_OBFKIE); }
    if (controller.ram[0] & 2) { ctl |= BIT(NPCX_HICTRL_OBFMIE); }
    hw->HICTRL = ctl;
    if (controller.ram[0] & 4) { hw->HIKMST |= BIT(NPCX_HIKMST_F0); }
    else { hw->HIKMST &= ~BIT(NPCX_HIKMST_F0); }
}
void lilac_keyboard_event(uint32_t data) {
    struct espi_evt_data_kbc *evt = (void *)&data;
    k_spinlock_key_t key = k_spin_lock(&lock);
    struct kbc_reg *hw = (void *)DT_REG_ADDR_BY_NAME(DT_NODELABEL(host_sub), kbc);
    if (evt->evt == HOST_KBC_EVT_IBF) {
        if (ready) { kbc_receive(&controller, evt->type, evt->data); flags(); }
        // Clear IBF only after status/type capture and command handling. The
        // board-scoped driver delta reads SHIKMDI instead of destructive HIKMDI.
        (void)hw->HIKMDI;
    } else {
        hw->HIKMST &= ~BIT(NPCX_HIKMST_ST1); // AUX OBF flag, stock lpc_clear_keyboard_buffer
    }
    flush();
    k_spin_unlock(&lock, key);
}
void lilac_keyboard_reset(bool enabled) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    struct kbc_reg *hw = (void *)DT_REG_ADDR_BY_NAME(DT_NODELABEL(host_sub), kbc);
    ready = enabled;
    kbc_init(&controller);
    hw->HICTRL |= BIT(NPCX_HICTRL_FW_OBF);
    if (enabled) { hw->HICTRL |= BIT(NPCX_HICTRL_IBFCIE); }
    else { hw->HICTRL &= ~BIT(NPCX_HICTRL_IBFCIE); }
    flags();
    k_spin_unlock(&lock, key);
}
void lilac_keyboard_power(bool allowed) {
    k_spinlock_key_t key = k_spin_lock(&deck_lock);
    deck_allowed = allowed;
    if (!allowed) {
        gpio_pin_set_raw(module_port, 6, 0);
        gpio_pin_set_raw(caps_port, 1, 0);
        deck_on = false; deck_state = 0;
    }
    k_spin_unlock(&deck_lock, key);
}
static void matrix_event(struct input_event *evt, void *unused) {
    ARG_UNUSED(unused);
    if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_X) { col = evt->value; }
    if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) { row = evt->value; }
    if (evt->type != INPUT_EV_KEY || evt->code != INPUT_BTN_TOUCH || row >= 8 || col >= 16) { return; }
    k_spinlock_key_t key = k_spin_lock(&lock);
    scan_events++;
    if (deck_on && ready) { kbc_key(&controller, scancodes[col][row], evt->value); flush(); }
    k_spin_unlock(&lock, key);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(kbd)), matrix_event, NULL);
uint16_t lilac_keyboard_map(unsigned r, unsigned c, bool write, uint16_t code) {
    if (r >= 8 || c >= 16) { return 0; }
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (write) { scancodes[c][r] = code; }
    uint16_t v = scancodes[c][r];
    k_spin_unlock(&lock, key);
    return v;
}
void lilac_keyboard_state(int32_t out[12]) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    out[0] = controller.received; out[1] = controller.sent;
    out[2] = controller.keys; out[3] = controller.dropped;
    out[4] = controller.ram[0]; out[5] = controller.enabled;
    out[6] = controller.unknown; out[7] = scan_events;
    k_spin_unlock(&lock, key);
    key = k_spin_lock(&deck_lock);
    out[8] = deck_state; out[9] = deck_mv; out[10] = deck_id; out[11] = deck_on;
    k_spin_unlock(&deck_lock, key);
}
// Dedicated bounded ADC poller: never stall fan/thermal work on the system queue.
static void deck_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        k_sleep(K_MSEC(10));
        k_spinlock_key_t key = k_spin_lock(&deck_lock);
        bool allowed = deck_allowed, powered = deck_on;
        k_spin_unlock(&deck_lock, key);
        if (!allowed) { continue; }
        int32_t mv = -1;
        int ret = 0;
        if (!powered) { ret = gpio_pin_set_raw(pug_port, 3, 1); }
        if (!ret) { ret = lilac_adc_read(3, &mv); }
        gpio_pin_set_raw(pug_port, 3, 0);
        bool present = !ret && deck_present(mv, powered);
        key = k_spin_lock(&deck_lock);
        deck_mv = ret ? -1 : mv; deck_id = ret ? -1 : deck_board_id(mv);
        if (deck_allowed) {
            if (!present) {
                gpio_pin_set_raw(module_port, 6, 0); deck_on = false; deck_state = 1;
            } else if (deck_state == 2 || powered) {
                if (!gpio_pin_set_raw(module_port, 6, 1)) { deck_on = true; deck_state = 3; }
            } else { deck_state = 2; } // stock two consecutive successful detections
        }
        bool caps_allowed = deck_on;
        k_spin_unlock(&deck_lock, key);
        key = k_spin_lock(&lock);
        bool caps = caps_allowed && (controller.leds & 4);
        flush();
        k_spin_unlock(&lock, key);
        key = k_spin_lock(&deck_lock);
        gpio_pin_set_raw(caps_port, 1, deck_allowed && caps);
        k_spin_unlock(&deck_lock, key);
    }
}
K_THREAD_DEFINE(lilac_deck, 1024, deck_thread, NULL, NULL, NULL, 10, 0, 0);
