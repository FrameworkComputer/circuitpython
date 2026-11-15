# Lilac AP-off diagnostic candidate

Build only; not hardware validated or approved for flashing.

`make BOARD=framework_lilac_diag` builds a separate NPCX993 target. Native
main configures stock-intent output levels before CircuitPython, and native
port reset reapplies them. Every pin is attempted even after an error; any
GPIO setup error then halts before Python. A pin whose setup failed keeps its
previous state, so this cannot guarantee a safe electrical state.

Filesystem initialization returns before touching flash; low-level flash
initialization cannot discover a flash area. No FAT partition is declared.
Raw GPIO/NVM modules and busio registration are excluded; UART internals remain
compiled for the native console. The board module contains inert pin names. No AP, charger, PD, cooling, host or
emergency protection services are implemented. Do not power the AP.

Pin source: ec-lilac 6bcd05acb6c11cc2a2e247118b9e741bf7e6d1f0,
zephyr/program/framework/lilac/gpio.dtsi. All stock output defaults are
preserved, including 1.8 V RSMRST, 1.8 V open-drain PBTN low and released
PROCHOT. Pin voltages, external pulls, reset/ROM gap and supply arrangement
require electrical review and measurement. Python reset handling does not
cover the time before native main or replace native emergency protection.
User confirmation is required before the first hardware test.
