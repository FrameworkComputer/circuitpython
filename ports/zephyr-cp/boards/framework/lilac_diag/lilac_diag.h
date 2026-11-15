// Native interface of the Lilac AP-off diagnostic target. Read-only: Python
// gets named input levels and allowlisted register reads, nothing else.
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
