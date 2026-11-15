// Native interface of the Lilac AP-off diagnostic target. Python gets named
// input levels, allowlisted register reads and one masked charger write path
// limited to allowlisted bits.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t lilac_input_count(void);
const char *lilac_input_name(size_t index);
// 0 or 1, or a negative errno.
int lilac_input_get(size_t index);

const uint8_t *lilac_charger_registers(size_t *count);
const uint8_t *lilac_thermal_registers(size_t *count);
// 0 on success; -EPERM for a register outside the allowlist; other negative
// errno on bus failure.
int lilac_charger_read(uint8_t reg, uint16_t *value);
int lilac_thermal_read(uint8_t reg, uint8_t *value);

struct lilac_writable { uint8_t reg; uint16_t mask; };
// (register, writable bit mask) pairs that lilac_charger_update() accepts.
const struct lilac_writable *lilac_charger_writable(size_t *count);
// Read-modify-write of `mask` bits to `value`, skipped if unchanged, then read
// back. -EPERM outside the writable list, -EINVAL for value bits outside mask,
// -EIO if the readback does not show the requested bits. *result = readback.
int lilac_charger_update(uint8_t reg, uint16_t mask, uint16_t value, uint16_t *result);

// Level of slp_s3_l, the stock power-good pin for the sensors and the fan
// (1 = S0), or a negative errno. lilac_thermal_read() returns -EAGAIN without
// bus access while it is low. The _quiet variant does not log failures.
int lilac_sensor_power(void);
int lilac_thermal_read_quiet(uint8_t reg, uint8_t *value);

// Fan (PWM0/GPC3, 25 kHz; tach TA2/GP73, 2 pulses per turn) and native
// thermal floor. Python requests a duty; native code applies 0 while the fan
// is unpowered and overrides with 100 % when Python has not refreshed its
// request for LILAC_FAN_LEASE_MS, when a sensor read fails, or when a sensor
// reaches its stock full-fan temperature.
#define LILAC_FAN_LEASE_MS 5000
#define LILAC_FAN_PERIOD_MS 1000
#define LILAC_TEMP_SENSORS 3
#define LILAC_TEMP_INVALID INT16_MIN

enum lilac_fan_reason {
    LILAC_FAN_UNPOWERED, // slp_s3_l low: duty 0, sensors not read
    LILAC_FAN_PYTHON,    // Python's request applies
    LILAC_FAN_LEASE,     // no request within the lease: 100 %
    LILAC_FAN_SENSOR,    // a sensor read failed: 100 %
    LILAC_FAN_HOT,       // a sensor at or above its full-fan limit: 100 %
};

struct lilac_temp_limit {
    const char *name;
    uint8_t reg;     // F75303 integer-degree register
    int16_t full_c;  // stock temp_fan_max: native forces 100 % at or above
    int16_t halt_c;  // stock temp_host_halt: latches `halt` at or above
    int16_t release_c; // stock temp_host_release_halt: clears `halt` below
};

struct lilac_fan_status {
    uint8_t request;  // last Python request, percent
    uint8_t duty;     // applied, percent
    bool requested;   // Python has made a request since boot
    bool powered;
    bool halt;        // a sensor reached halt_c; no shutdown path exists yet
    int pwm_error;    // 0 or the last PWM error
    int32_t rpm;      // tach reading, or a negative errno
    enum lilac_fan_reason reason;
    int16_t temp_c[LILAC_TEMP_SENSORS]; // LILAC_TEMP_INVALID if not read
    int64_t updated_ms;
};

const struct lilac_temp_limit *lilac_temp_limits(size_t *count);
// 0..100; -EINVAL otherwise. Refreshes the lease and applies at once.
int lilac_fan_set(int percent);
void lilac_fan_status(struct lilac_fan_status *status);
void lilac_fan_start(void);

// eSPI host interface (espi.c), configured as stock at boot independent of
// Python. Host commands go to Python through one slot: Python polls, then
// answers; without an answer within LILAC_HC_TIMEOUT_MS native code answers
// EC_RES_TIMEOUT. eSPI or platform reset drops a pending request.
#define LILAC_HC_TIMEOUT_MS 100
#define LILAC_HC_DATA_MAX 248

struct lilac_espi_status {
    int config_result;   // 0 once configured, else negative errno
    bool vw_ready;       // VW channel ready since the last eSPI reset
    int8_t espi_rst;     // last eSPI_RST# level, -1 if no edge seen
    int8_t pltrst;       // last PLTRST# VW level, -1 if never received
    uint32_t espi_resets, pltrst_edges;
    uint32_t hc_requests, hc_answered, hc_native, hc_bad, hc_busy, hc_timeouts,
        hc_late, hc_aborted, hc_unknown_protocol;
    uint32_t acpi_bytes, acpi_queries, acpi_ignored_reads, acpi_ignored_writes;
    uint32_t kbc_bytes;
    uint32_t port80_total, port80_dropped, port80_last;
};

struct lilac_hc_request {
    uint32_t seq;
    uint16_t command;
    uint8_t version;
    uint16_t data_len;
    uint8_t data[LILAC_HC_DATA_MAX];
};

void lilac_espi_status(struct lilac_espi_status *status);
// Moves up to max buffered Port 80 codes into out, oldest first.
size_t lilac_port80_drain(uint32_t *out, size_t max);
// 0 and the pending request, or -EAGAIN if none is pending.
int lilac_hc_poll(struct lilac_hc_request *req);
// Answer request `seq`; -ETIMEDOUT if it was already answered (timeout) or
// dropped, -EINVAL if data is longer than LILAC_HC_DATA_MAX.
int lilac_hc_respond(uint32_t seq, uint16_t result, const uint8_t *data, size_t len);
