# Lilac AP-off diagnostic candidate

Run on Lilac hardware (2026-11-15, adapter only, no battery, AP off), through
`dd6e647430`: UART REPL, 28/28 pins configured, inputs match the stock G3
state, charger reads and the one masked write work, littlefs storage formats
and persists, 10 GC/exception/Ctrl-C/Ctrl-D reload cycles and a 20-minute soak
were clean. The F75303 sensor bus is not idle in G3. No rail, current or
temperature measurement was taken with instruments; not validated beyond that.
Evidence and the bring-up plan live outside this repository.

`make BOARD=framework_lilac_diag` builds a separate NPCX993 target. Native
main configures stock-intent output levels before CircuitPython, and native
port reset reapplies them. Every pin is attempted even after an error; any
GPIO setup error then halts before Python. A pin whose setup failed keeps its
previous state, so this cannot guarantee a safe electrical state.

After the first successful setup, native code prints two `Lilac diag:` lines:
the level read back on each configured pad in table order (with a count of
pads driven low that do not read low) and the 3V/5V power-good, VR power-good,
SLP_S3 and SLP_S5 inputs. These are read-only observations, not protection.

Read-only visibility (since `141b873a06`): the `lilac`
Python module exposes `inputs()`/`input(name)` for 21 named inputs (configured
with their stock flags: input, no pulls, 1.8 V detection on `ec_espi_rst_l`),
and `charger_read(reg)` / `thermal_read(reg)` for fixed register allowlists of
the ISL9241 charger (I2C3, 0x09, SMBus word) and F75303 thermal sensor (I2C7,
0x4D, byte). Registers outside the allowlist raise `ValueError` before any bus
access. The only write path is `charger_update(reg, mask, value)`, limited to
the bits listed by `charger_writable()` — currently ISL9241 CONTROL3 (0x4C)
bit 0, ADC always on. It is a read-modify-write like the stock driver's
`isl9241_update()`, skipped if nothing changes, and verified by read-back. `BOARD_BUS_OBJECTS = false` keeps the
generated `board` module from exposing `busio.I2C` objects for these buses.
Native code also runs a battery-LED heartbeat (green PWM at the stock 16 %
level, 100 ms every 2 s); for that, the `left_side`/`right_side` LED selects
are driven high, a deliberate deviation from the stock initial low. The
heartbeat shows the kernel work queue is alive, not Python. Zephyr minimal
logging prints driver warnings and failed reads on the console.

Storage (image 4 onward, `LILAC_STORAGE = true`): littlefs CIRCUITPY in the
fixed partition `0x41000..0x7ffff` (252 KiB); the build fails if that
partition is missing, so runtime flash discovery can never pick the firmware
region. Python writes files with `open()`/`os`; there is no USB drive, files
are copied over the UART REPL. The first boot formats the partition. The
console UART receive buffer is 1 KiB (`CONSOLE_UART_RX_BUFFER_SIZE`).
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
