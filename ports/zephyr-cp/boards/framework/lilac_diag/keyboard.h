#pragma once
#include <stdbool.h>
#include <stdint.h>
void lilac_keyboard_event(uint32_t data);
void lilac_keyboard_reset(bool ready);
void lilac_keyboard_power(bool allowed);
uint16_t lilac_keyboard_map(unsigned row, unsigned col, bool write, uint16_t code);
void lilac_keyboard_state(int32_t out[12]);
