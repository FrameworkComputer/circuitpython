// Native interface of the Lilac AP-off diagnostic target. Python gets named
// input levels, allowlisted register reads and one masked charger write path
// limited to allowlisted bits.
#pragma once

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
