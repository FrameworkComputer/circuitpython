// `lilac` module for the Lilac AP-off diagnostic target. Thin wrappers over
// native mechanisms; all hardware access and its allowlists live in diagnostic.c.
#include <errno.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include <zephyr/kernel.h>

#include "boards/framework/lilac_diag/lilac_diag.h"

static void lilac_check(int ret) {
    if (ret == -EPERM) {
        mp_raise_ValueError(MP_ERROR_TEXT("register not in allowlist"));
    }
    if (ret < 0) {
        mp_raise_OSError(-ret);
    }
}

//| def inputs() -> dict[str, int]:
//|     """Levels of all named EC inputs."""
static mp_obj_t lilac_inputs(void) {
    mp_obj_t dict = mp_obj_new_dict(lilac_input_count());
    for (size_t i = 0; i < lilac_input_count(); i++) {
        int level = lilac_input_get(i);
        lilac_check(level);
        const char *name = lilac_input_name(i);
        mp_obj_dict_store(dict, mp_obj_new_str(name, strlen(name)), MP_OBJ_NEW_SMALL_INT(level));
    }
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_inputs_obj, lilac_inputs);

//| def input(name: str) -> int:
//|     """Level of one named EC input."""
static mp_obj_t lilac_input(mp_obj_t name_in) {
    const char *name = mp_obj_str_get_str(name_in);
    for (size_t i = 0; i < lilac_input_count(); i++) {
        if (strcmp(name, lilac_input_name(i)) == 0) {
            int level = lilac_input_get(i);
            lilac_check(level);
            return MP_OBJ_NEW_SMALL_INT(level);
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("unknown input"));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lilac_input_obj, lilac_input);

static mp_obj_t lilac_reg_tuple(const uint8_t *regs, size_t count) {
    mp_obj_t items[count];
    for (size_t i = 0; i < count; i++) {
        items[i] = MP_OBJ_NEW_SMALL_INT(regs[i]);
    }
    return mp_obj_new_tuple(count, items);
}

static uint8_t lilac_reg_arg(mp_obj_t reg_in) {
    mp_int_t reg = mp_obj_get_int(reg_in);
    if (reg < 0 || reg > 0xff) {
        mp_raise_ValueError(MP_ERROR_TEXT("register not in allowlist"));
    }
    return (uint8_t)reg;
}

//| def charger_registers() -> tuple[int, ...]:
//|     """ISL9241 registers that charger_read() accepts."""
static mp_obj_t lilac_charger_registers_fn(void) {
    size_t count;
    const uint8_t *regs = lilac_charger_registers(&count);
    return lilac_reg_tuple(regs, count);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_charger_registers_obj, lilac_charger_registers_fn);

//| def charger_read(reg: int) -> int:
//|     """Read one 16-bit ISL9241 register (SMBus word)."""
static mp_obj_t lilac_charger_read_fn(mp_obj_t reg_in) {
    uint16_t value;
    lilac_check(lilac_charger_read(lilac_reg_arg(reg_in), &value));
    return MP_OBJ_NEW_SMALL_INT(value);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lilac_charger_read_obj, lilac_charger_read_fn);

//| def charger_writable() -> tuple[tuple[int, int], ...]:
//|     """(register, bit mask) pairs that charger_update() may change."""
static mp_obj_t lilac_charger_writable_fn(void) {
    size_t count;
    const struct lilac_writable *w = lilac_charger_writable(&count);
    mp_obj_t items[count];
    for (size_t i = 0; i < count; i++) {
        mp_obj_t pair[2] = { MP_OBJ_NEW_SMALL_INT(w[i].reg), MP_OBJ_NEW_SMALL_INT(w[i].mask) };
        items[i] = mp_obj_new_tuple(2, pair);
    }
    return mp_obj_new_tuple(count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_charger_writable_obj, lilac_charger_writable_fn);

//| def charger_update(reg: int, mask: int, value: int) -> int:
//|     """Set the `mask` bits of an ISL9241 register to `value` (read-modify-
//|     write, skipped if unchanged) and return the read-back word. Only the
//|     bits listed by charger_writable() are accepted."""
static mp_obj_t lilac_charger_update_fn(mp_obj_t reg_in, mp_obj_t mask_in, mp_obj_t value_in) {
    mp_int_t mask = mp_obj_get_int(mask_in);
    mp_int_t value = mp_obj_get_int(value_in);
    if (mask <= 0 || mask > 0xffff || value < 0 || value > 0xffff) {
        mp_raise_ValueError(MP_ERROR_TEXT("register not in allowlist"));
    }
    uint16_t result;
    int ret = lilac_charger_update(lilac_reg_arg(reg_in), (uint16_t)mask, (uint16_t)value, &result);
    if (ret == -EINVAL) {
        mp_raise_ValueError(MP_ERROR_TEXT("value outside mask"));
    }
    lilac_check(ret);
    return MP_OBJ_NEW_SMALL_INT(result);
}
static MP_DEFINE_CONST_FUN_OBJ_3(lilac_charger_update_obj, lilac_charger_update_fn);

//| def thermal_registers() -> tuple[int, ...]:
//|     """F75303 registers that thermal_read() accepts."""
static mp_obj_t lilac_thermal_registers_fn(void) {
    size_t count;
    const uint8_t *regs = lilac_thermal_registers(&count);
    return lilac_reg_tuple(regs, count);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_thermal_registers_obj, lilac_thermal_registers_fn);

//| def thermal_read(reg: int) -> int:
//|     """Read one 8-bit F75303 register. Raises OSError(EAGAIN) without bus
//|     access while the sensor is unpowered (slp_s3_l low)."""
static mp_obj_t lilac_thermal_read_fn(mp_obj_t reg_in) {
    uint8_t value;
    lilac_check(lilac_thermal_read(lilac_reg_arg(reg_in), &value));
    return MP_OBJ_NEW_SMALL_INT(value);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lilac_thermal_read_obj, lilac_thermal_read_fn);

//| def fan_set(percent: int) -> None:
//|     """Request a fan duty of 0..100 %. Native code applies it while the fan
//|     is powered (S0), no sensor has failed or reached its full-fan limit,
//|     and the request is refreshed at least every 5 s; otherwise it applies
//|     0 % (unpowered) or 100 %."""
static mp_obj_t lilac_fan_set_fn(mp_obj_t percent_in) {
    mp_int_t percent = mp_obj_get_int(percent_in);
    if (percent < 0 || percent > 100) {
        mp_raise_ValueError(MP_ERROR_TEXT("duty outside 0..100"));
    }
    lilac_check(lilac_fan_set((int)percent));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lilac_fan_set_obj, lilac_fan_set_fn);

static void lilac_store(mp_obj_t dict, const char *key, mp_obj_t value) {
    mp_obj_dict_store(dict, mp_obj_new_str(key, strlen(key)), value);
}

//| def fan_status() -> dict:
//|     """Native fan and thermal state from the last 1 s update: request,
//|     duty, rpm (None on tach error), powered, reason ('unpowered',
//|     'python', 'lease', 'sensor', 'hot'), halt, pwm_error, temps
//|     (name -> degrees C, None if not read) and age_ms."""
static mp_obj_t lilac_fan_status_fn(void) {
    static const char *const reasons[] = { "unpowered", "python", "lease", "sensor", "hot" };
    struct lilac_fan_status st;
    lilac_fan_status(&st);
    size_t count;
    const struct lilac_temp_limit *limits = lilac_temp_limits(&count);
    mp_obj_t temps = mp_obj_new_dict(count);
    for (size_t i = 0; i < count; i++) {
        lilac_store(temps, limits[i].name, st.temp_c[i] == LILAC_TEMP_INVALID
            ? mp_const_none : MP_OBJ_NEW_SMALL_INT(st.temp_c[i]));
    }
    mp_obj_t dict = mp_obj_new_dict(10);
    lilac_store(dict, "request", MP_OBJ_NEW_SMALL_INT(st.request));
    lilac_store(dict, "requested", mp_obj_new_bool(st.requested));
    lilac_store(dict, "duty", MP_OBJ_NEW_SMALL_INT(st.duty));
    lilac_store(dict, "rpm", st.rpm < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(st.rpm));
    lilac_store(dict, "powered", mp_obj_new_bool(st.powered));
    const char *reason = reasons[st.reason];
    lilac_store(dict, "reason", mp_obj_new_str(reason, strlen(reason)));
    lilac_store(dict, "halt", mp_obj_new_bool(st.halt));
    lilac_store(dict, "pwm_error", MP_OBJ_NEW_SMALL_INT(st.pwm_error));
    lilac_store(dict, "temps", temps);
    lilac_store(dict, "age_ms", mp_obj_new_int_from_ll(k_uptime_get() - st.updated_ms));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_fan_status_obj, lilac_fan_status_fn);

//| def temp_limits() -> tuple[tuple[str, int, int, int, int], ...]:
//|     """Native limits per sensor: (name, F75303 register, full-fan C,
//|     halt C, halt release C), from the stock Lilac thermal table."""
static mp_obj_t lilac_temp_limits_fn(void) {
    size_t count;
    const struct lilac_temp_limit *limits = lilac_temp_limits(&count);
    mp_obj_t items[count];
    for (size_t i = 0; i < count; i++) {
        mp_obj_t entry[5] = {
            mp_obj_new_str(limits[i].name, strlen(limits[i].name)),
            MP_OBJ_NEW_SMALL_INT(limits[i].reg),
            MP_OBJ_NEW_SMALL_INT(limits[i].full_c),
            MP_OBJ_NEW_SMALL_INT(limits[i].halt_c),
            MP_OBJ_NEW_SMALL_INT(limits[i].release_c),
        };
        items[i] = mp_obj_new_tuple(5, entry);
    }
    return mp_obj_new_tuple(count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_temp_limits_obj, lilac_temp_limits_fn);

static const mp_rom_map_elem_t lilac_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_lilac) },
    { MP_ROM_QSTR(MP_QSTR_inputs), MP_ROM_PTR(&lilac_inputs_obj) },
    { MP_ROM_QSTR(MP_QSTR_input), MP_ROM_PTR(&lilac_input_obj) },
    { MP_ROM_QSTR(MP_QSTR_charger_registers), MP_ROM_PTR(&lilac_charger_registers_obj) },
    { MP_ROM_QSTR(MP_QSTR_charger_read), MP_ROM_PTR(&lilac_charger_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_charger_writable), MP_ROM_PTR(&lilac_charger_writable_obj) },
    { MP_ROM_QSTR(MP_QSTR_charger_update), MP_ROM_PTR(&lilac_charger_update_obj) },
    { MP_ROM_QSTR(MP_QSTR_thermal_registers), MP_ROM_PTR(&lilac_thermal_registers_obj) },
    { MP_ROM_QSTR(MP_QSTR_thermal_read), MP_ROM_PTR(&lilac_thermal_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_fan_set), MP_ROM_PTR(&lilac_fan_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_fan_status), MP_ROM_PTR(&lilac_fan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_temp_limits), MP_ROM_PTR(&lilac_temp_limits_obj) },
};
static MP_DEFINE_CONST_DICT(lilac_module_globals, lilac_module_globals_table);

const mp_obj_module_t lilac_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lilac_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lilac, lilac_module);
