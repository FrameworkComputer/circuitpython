// `lilac` module for the Lilac AP-off diagnostic target. Thin wrappers over
// native mechanisms; all hardware access and its allowlists live in diagnostic.c.
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include <zephyr/kernel.h>

#include "boards/framework/lilac_diag/lilac_diag.h"
#include "supervisor/port_heap.h"

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

//| def charger_writable() -> tuple[tuple[int, int, int, int], ...]:
//|     """(register, bit mask, min, max) entries that charger_update() may
//|     change. Unless min is 0 and max equals the mask, the whole mask must be
//|     written with a value in [min, max] (else OSError ERANGE)."""
static mp_obj_t lilac_charger_writable_fn(void) {
    size_t count;
    const struct lilac_writable *w = lilac_charger_writable(&count);
    mp_obj_t items[count];
    for (size_t i = 0; i < count; i++) {
        mp_obj_t entry[4] = {
            MP_OBJ_NEW_SMALL_INT(w[i].reg), MP_OBJ_NEW_SMALL_INT(w[i].mask),
            MP_OBJ_NEW_SMALL_INT(w[i].min), MP_OBJ_NEW_SMALL_INT(w[i].max),
        };
        items[i] = mp_obj_new_tuple(4, entry);
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

//| def espi_status() -> dict:
//|     """Native eSPI state and counters: configured (0 or -errno), vw_ready,
//|     espi_rst and pltrst (last level, None before the first edge), reset
//|     and PLTRST edge counts, host command counters (hc_*), ACPI byte
//|     counters (acpi_*), kbc_bytes and port80 total/dropped/last."""
static mp_obj_t lilac_espi_status_fn(void) {
    struct lilac_espi_status st;
    lilac_espi_status(&st);
    mp_obj_t dict = mp_obj_new_dict(26);
    lilac_store(dict, "configured", MP_OBJ_NEW_SMALL_INT(st.config_result));
    lilac_store(dict, "vw_ready", mp_obj_new_bool(st.vw_ready));
    lilac_store(dict, "backlight", mp_obj_new_bool(st.backlight));
    lilac_store(dict, "espi_rst", st.espi_rst < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(st.espi_rst));
    lilac_store(dict, "pltrst", st.pltrst < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(st.pltrst));
    static const struct { const char *name; size_t offset; } counters[] = {
        #define C(f) { #f, offsetof(struct lilac_espi_status, f) }
        C(espi_resets), C(pltrst_edges),
        C(hc_requests), C(hc_answered), C(hc_native), C(hc_bad), C(hc_busy),
        C(hc_timeouts), C(hc_late), C(hc_aborted), C(hc_unknown_protocol),
        C(acpi_bytes), C(acpi_queries), C(acpi_ignored_reads), C(acpi_ignored_writes),
        C(kbc_bytes), C(port80_total), C(port80_dropped), C(port80_last),
        #undef C
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(counters); i++) {
        uint32_t v = *(const uint32_t *)((const uint8_t *)&st + counters[i].offset);
        lilac_store(dict, counters[i].name, mp_obj_new_int_from_uint(v));
    }
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_espi_status_obj, lilac_espi_status_fn);

//| def port80() -> list[int]:
//|     """Drain buffered Port 80 codes (up to 64, oldest first)."""
static mp_obj_t lilac_port80_fn(void) {
    uint32_t codes[64];
    size_t n = lilac_port80_drain(codes, MP_ARRAY_SIZE(codes));
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < n; i++) {
        mp_obj_list_append(list, mp_obj_new_int_from_uint(codes[i]));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_port80_obj, lilac_port80_fn);

//| def hc_poll() -> tuple[int, int, int, bytes] | None:
//|     """The pending host command as (seq, command, version, params), or
//|     None. Answer with hc_respond() within 100 ms; otherwise native code
//|     answers EC_RES_TIMEOUT."""
static mp_obj_t lilac_hc_poll_fn(void) {
    static struct lilac_hc_request req;
    if (lilac_hc_poll(&req) != 0) {
        return mp_const_none;
    }
    mp_obj_t items[4] = {
        mp_obj_new_int_from_uint(req.seq),
        MP_OBJ_NEW_SMALL_INT(req.command),
        MP_OBJ_NEW_SMALL_INT(req.version),
        mp_obj_new_bytes(req.data, req.data_len),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_hc_poll_obj, lilac_hc_poll_fn);

//| def hc_respond(seq: int, result: int, data: bytes = b"") -> None:
//|     """Answer host command `seq` with an ec_status and response data (at
//|     most 248 bytes; error results send no data). Raises OSError(EINVAL) for
//|     a result outside 0..0xffff or longer data, OSError(ETIMEDOUT) if native
//|     code already answered or dropped the request."""
static mp_obj_t lilac_hc_respond_fn(size_t n_args, const mp_obj_t *args) {
    mp_int_t result = mp_obj_get_int(args[1]);
    if (result < 0 || result > 0xffff) {
        mp_raise_OSError(EINVAL);
    }
    mp_buffer_info_t buf = { .buf = NULL, .len = 0 };
    if (n_args > 2) {
        mp_get_buffer_raise(args[2], &buf, MP_BUFFER_READ);
    }
    if (buf.len > LILAC_HC_DATA_MAX) {
        mp_raise_OSError(EINVAL);
    }
    lilac_check(lilac_hc_respond((uint32_t)mp_obj_get_int_truncated(args[0]), (uint16_t)result,
        buf.buf, buf.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lilac_hc_respond_obj, 2, 3, lilac_hc_respond_fn);

//| def adc_channels() -> tuple[str, ...]:
//|     """Names of the ADC channels adc_read() accepts (stock channels)."""
static mp_obj_t lilac_adc_channels_fn(void) {
    size_t count;
    const struct lilac_adc_channel *ch = lilac_adc_channels(&count);
    mp_obj_t items[count];
    for (size_t i = 0; i < count; i++) {
        items[i] = mp_obj_new_str(ch[i].name, strlen(ch[i].name));
    }
    return mp_obj_new_tuple(count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_adc_channels_obj, lilac_adc_channels_fn);

//| def adc_read(name: str) -> int:
//|     """One conversion of a named ADC channel, in millivolts."""
static mp_obj_t lilac_adc_read_fn(mp_obj_t name_in) {
    const char *name = mp_obj_str_get_str(name_in);
    size_t count;
    const struct lilac_adc_channel *ch = lilac_adc_channels(&count);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, ch[i].name) == 0) {
            int32_t mv;
            int ret = lilac_adc_read(i, &mv);
            if (ret < 0) {
                mp_raise_OSError(-ret);
            }
            return mp_obj_new_int(mv);
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("unknown ADC channel"));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lilac_adc_read_obj, lilac_adc_read_fn);

static size_t memmap_range(mp_obj_t offset_in, mp_int_t len) {
    mp_int_t offset = mp_obj_get_int(offset_in);
    if (offset < 0 || len < 0 || offset > LILAC_MEMMAP_SIZE || len > LILAC_MEMMAP_SIZE - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("outside memmap"));
    }
    return (size_t)offset;
}

//| def memmap_read(offset: int, length: int) -> bytes:
//|     """Bytes of the 512-byte host shared memory window (ACPI SHM 0xe00)."""
static mp_obj_t lilac_memmap_read_fn(mp_obj_t offset_in, mp_obj_t len_in) {
    mp_int_t len = mp_obj_get_int(len_in);
    size_t offset = memmap_range(offset_in, len);
    uint8_t buf[LILAC_MEMMAP_SIZE];
    int ret = lilac_memmap_read(offset, buf, (size_t)len);
    if (ret < 0) {
        mp_raise_OSError(-ret);
    }
    return mp_obj_new_bytes(buf, (size_t)len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(lilac_memmap_read_obj, lilac_memmap_read_fn);

//| def memmap_write(offset: int, data: bytes, mask: int = 0xff) -> None:
//|     """Set the `mask` bits of each byte from `data`. Native-owned bytes
//|     (host-command flags at 0x27) raise OSError EPERM, nothing written."""
static mp_obj_t lilac_memmap_write_fn(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_READ);
    size_t offset = memmap_range(args[0], (mp_int_t)buf.len);
    mp_int_t mask = n_args > 2 ? mp_obj_get_int(args[2]) : 0xff;
    if (mask < 0 || mask > 0xff) {
        mp_raise_ValueError(MP_ERROR_TEXT("mask outside 0..0xff"));
    }
    int ret = lilac_memmap_update(offset, buf.buf, buf.len, (uint8_t)mask);
    if (ret < 0) {
        mp_raise_OSError(-ret);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lilac_memmap_write_obj, 2, 3, lilac_memmap_write_fn);

// Power calls report every refusal as OSError(errno); EPERM means disarmed.
static void power_check(int ret) {
    if (ret < 0) {
        mp_raise_OSError(-ret);
    }
}

//| def power_set(name: str, level: int) -> None:
//|     """Request one AP power output (names from power_status()). Raises
//|     OSError EPERM (disarmed build), EBUSY (fault latched or powering
//|     down), EAGAIN (stock predecessor, delay or power good missing; retry)
//|     or EINVAL. Lowering is always accepted. Refreshes the 1 s lease."""
static mp_obj_t lilac_power_set_fn(mp_obj_t name_in, mp_obj_t level_in) {
    power_check(lilac_power_set(mp_obj_str_get_str(name_in), mp_obj_get_int(level_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lilac_power_set_obj, lilac_power_set_fn);

//| def power_pbtn_pulse() -> None:
//|     """Stock 20 ms power-button press (needs RSMRST released for 90 ms)."""
static mp_obj_t lilac_power_pbtn_pulse_fn(void) {
    power_check(lilac_power_pbtn_pulse());
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_power_pbtn_pulse_obj, lilac_power_pbtn_pulse_fn);

//| def power_heartbeat() -> None:
//|     """Refresh the lease; with rails up, native code switches everything
//|     off (fault 'lease') if this is not called for 1 s."""
static mp_obj_t lilac_power_heartbeat_fn(void) {
    lilac_power_heartbeat();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_power_heartbeat_obj, lilac_power_heartbeat_fn);

//| def power_off() -> None:
//|     """All AP power outputs off at once in stock force_g3 order."""
static mp_obj_t lilac_power_off_fn(void) {
    lilac_power_off();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_power_off_obj, lilac_power_off_fn);

//| def power_clear_fault() -> None:
//|     """Clear a latched fault (OSError EBUSY while any output is high)."""
static mp_obj_t lilac_power_clear_fault_fn(void) {
    power_check(lilac_power_clear_fault());
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_power_clear_fault_obj, lilac_power_clear_fault_fn);

//| def power_status() -> dict:
//|     """armed, fault ('none' or latched reason), faults, fault_ms,
//|     outputs (name -> level), inputs (pg_3v5v, pg_vr, slp_s3_l, slp_s5_l
//|     as last sampled by the 1 ms monitor), vw_ready, lease_ms, ticks,
//|     write_errors, input_errors."""
static mp_obj_t lilac_power_status_fn(void) {
    static const char *const in_names[LILAC_POWER_INPUTS] = { "pg_3v5v", "pg_vr", "slp_s3_l", "slp_s5_l" };
    struct lilac_power_status st;
    lilac_power_status(&st);
    mp_obj_t outputs = mp_obj_new_dict(LILAC_POWER_OUTPUTS);
    for (int i = 0; i < LILAC_POWER_OUTPUTS; i++) {
        lilac_store(outputs, lilac_power_out_name(i), MP_OBJ_NEW_SMALL_INT(st.out[i]));
    }
    mp_obj_t inputs = mp_obj_new_dict(LILAC_POWER_INPUTS);
    for (int i = 0; i < LILAC_POWER_INPUTS; i++) {
        lilac_store(inputs, in_names[i], MP_OBJ_NEW_SMALL_INT(st.in[i]));
    }
    mp_obj_t dict = mp_obj_new_dict(12);
    lilac_store(dict, "armed", mp_obj_new_bool(st.armed));
    lilac_store(dict, "fault", mp_obj_new_str(st.fault, strlen(st.fault)));
    lilac_store(dict, "faults", mp_obj_new_int_from_uint(st.faults));
    lilac_store(dict, "fault_ms", mp_obj_new_int_from_ll(st.fault_ms));
    lilac_store(dict, "outputs", outputs);
    lilac_store(dict, "inputs", inputs);
    lilac_store(dict, "vw_ready", mp_obj_new_bool(st.vw_ready));
    lilac_store(dict, "lease_ms", mp_obj_new_int(st.lease_ms));
    lilac_store(dict, "ticks", mp_obj_new_int_from_uint(st.ticks));
    lilac_store(dict, "write_errors", mp_obj_new_int_from_uint(st.write_errors));
    lilac_store(dict, "input_errors", mp_obj_new_int_from_uint(st.input_errors));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_power_status_obj, lilac_power_status_fn);

// Image 10: allocation-light status calls for the S0 service loop. The
// dict-returning calls above build string keys at runtime (~0.7 ms and a
// heap object each at the stock 15 MHz core clock); in S0 their churn
// exhausted the fragmented split heap (runs 2, 3, 3b).

//| def power_names() -> tuple[str, ...]:
//|     """Power output names in power_state() bit order (call once)."""
static mp_obj_t lilac_power_names_fn(void) {
    mp_obj_t items[LILAC_POWER_OUTPUTS];
    for (int i = 0; i < LILAC_POWER_OUTPUTS; i++) {
        const char *name = lilac_power_out_name(i);
        items[i] = mp_obj_new_str(name, strlen(name));
    }
    return mp_obj_new_tuple(LILAC_POWER_OUTPUTS, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_power_names_obj, lilac_power_names_fn);

//| def power_state() -> tuple[int, int, int, int]:
//|     """(faulted 0/1, outputs bitmask in power_names() order, inputs
//|     bitmask pg_3v5v=1 pg_vr=2 slp_s3_l=4 slp_s5_l=8, plus 16 while the
//|     S0 peripherals (panel backlight enable) are on, lease_ms).
//|     Small ints only; power_status() names the fault."""
static mp_obj_t lilac_power_state_fn(void) {
    struct lilac_power_status st;
    lilac_power_status(&st);
    mp_int_t out = 0, in = 0;
    for (int i = 0; i < LILAC_POWER_OUTPUTS; i++) {
        out |= (mp_int_t)(st.out[i] ? 1 : 0) << i;
    }
    for (int i = 0; i < LILAC_POWER_INPUTS; i++) {
        in |= (mp_int_t)(st.in[i] ? 1 : 0) << i;
    }
    if (st.backlight) {
        in |= 1 << LILAC_POWER_INPUTS;
    }
    mp_obj_t items[4] = {
        MP_OBJ_NEW_SMALL_INT(strcmp(st.fault, "none") != 0),
        MP_OBJ_NEW_SMALL_INT(out),
        MP_OBJ_NEW_SMALL_INT(in),
        MP_OBJ_NEW_SMALL_INT(st.lease_ms),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_power_state_obj, lilac_power_state_fn);

//| def fan_state() -> tuple:
//|     """(powered, duty, rpm (-1 on tach error), reason index into
//|     ('unpowered', 'python', 'lease', 'sensor', 'hot'), halt, then one
//|     temperature per temp_limits() sensor, None if not read)."""
static mp_obj_t lilac_fan_state_fn(void) {
    struct lilac_fan_status st;
    lilac_fan_status(&st);
    mp_obj_t items[5 + LILAC_TEMP_SENSORS] = {
        mp_obj_new_bool(st.powered),
        MP_OBJ_NEW_SMALL_INT(st.duty),
        MP_OBJ_NEW_SMALL_INT(st.rpm < 0 ? -1 : st.rpm),
        MP_OBJ_NEW_SMALL_INT(st.reason),
        mp_obj_new_bool(st.halt),
    };
    for (int i = 0; i < LILAC_TEMP_SENSORS; i++) {
        items[5 + i] = st.temp_c[i] == LILAC_TEMP_INVALID ? mp_const_none : MP_OBJ_NEW_SMALL_INT(st.temp_c[i]);
    }
    return mp_obj_new_tuple(5 + LILAC_TEMP_SENSORS, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_fan_state_obj, lilac_fan_state_fn);

//| def port80_last() -> tuple[int, int]:
//|     """(total codes seen, most recent code) without draining."""
static mp_obj_t lilac_port80_last_fn(void) {
    uint32_t total, last;
    lilac_port80_last(&total, &last);
    mp_obj_t items[2] = { mp_obj_new_int_from_uint(total), mp_obj_new_int_from_uint(last) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_port80_last_obj, lilac_port80_last_fn);

//| def heap_info() -> int:
//|     """Largest free block of the outer (port) heap from which the VM
//|     heap grows, in bytes. Diagnostic."""
static mp_obj_t lilac_heap_info_fn(void) {
    return mp_obj_new_int_from_uint(port_heap_get_largest_free_size());
}
static MP_DEFINE_CONST_FUN_OBJ_0(lilac_heap_info_obj, lilac_heap_info_fn);

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
    { MP_ROM_QSTR(MP_QSTR_espi_status), MP_ROM_PTR(&lilac_espi_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_port80), MP_ROM_PTR(&lilac_port80_obj) },
    { MP_ROM_QSTR(MP_QSTR_hc_poll), MP_ROM_PTR(&lilac_hc_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_hc_respond), MP_ROM_PTR(&lilac_hc_respond_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_set), MP_ROM_PTR(&lilac_power_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_pbtn_pulse), MP_ROM_PTR(&lilac_power_pbtn_pulse_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_heartbeat), MP_ROM_PTR(&lilac_power_heartbeat_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_off), MP_ROM_PTR(&lilac_power_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_clear_fault), MP_ROM_PTR(&lilac_power_clear_fault_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_status), MP_ROM_PTR(&lilac_power_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_adc_channels), MP_ROM_PTR(&lilac_adc_channels_obj) },
    { MP_ROM_QSTR(MP_QSTR_adc_read), MP_ROM_PTR(&lilac_adc_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_memmap_read), MP_ROM_PTR(&lilac_memmap_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_memmap_write), MP_ROM_PTR(&lilac_memmap_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_names), MP_ROM_PTR(&lilac_power_names_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_state), MP_ROM_PTR(&lilac_power_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_fan_state), MP_ROM_PTR(&lilac_fan_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_port80_last), MP_ROM_PTR(&lilac_port80_last_obj) },
    { MP_ROM_QSTR(MP_QSTR_heap_info), MP_ROM_PTR(&lilac_heap_info_obj) },
};
static MP_DEFINE_CONST_DICT(lilac_module_globals, lilac_module_globals_table);

const mp_obj_module_t lilac_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lilac_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lilac, lilac_module);
