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
Fan and thermal floor (`b5cf28695d`, run on Lilac in G3 only, 2026-11-15):
PWM0/GPC3 and tach TA2/GP73 as stock. A native 1 s monitor on the system
work queue applies duty 0 while `slp_s3_l` (stock power-good pin of fan and
sensors) is low; in S0 it applies `lilac.fan_set()`'s request, or 100 % when
that request is older than 5 s, an F75303 read fails, or a sensor reaches
its stock full-fan temperature. The stock 120 C halt is latched and logged;
there is no shutdown path. `thermal_read()` raises EAGAIN without bus access
while `slp_s3_l` is low. `lilac.fan_status()` and `temp_limits()` report.

eSPI host interface (`7a0d2e85b6`, run on Lilac in G3 only, 2026-11-15):
`espi0` with stock pinctrl and stock Kconfig (ACPI SHM 512 B at 0xe00,
multi-byte Port 80). `espi.c` configures it at boot like the stock shim and
applies two Framework zephyr-ec deltas from board code (VW index 0x33 on
VWEVMS[8], ACPI SHM window writable by the host). `hostif.c` holds the
protocol logic: protocol 3 packet validation and responses as stock, one
host command at a time handed to Python (`lilac.hc_poll()` /
`lilac.hc_respond()`), answered EC_RES_TIMEOUT natively after 100 ms and
dropped on eSPI or platform reset; GET_PROTOCOL_INFO answered natively;
ACPI EC framing for the generic addresses with SCI on GPIO `ec_sci_l`;
Port 80 buffered for `lilac.port80()`. 8042 bytes are consumed and counted,
no keyboard controller. `lilac.espi_status()` reports state and counters.

AP power control (`9a56f57fa8`, run on Lilac in G3 only, 2026-11-15): `powerctl.c`
holds stock power_sequence.c's ordering as interlocks (each output only
after its predecessor, stock delay and power good; SYS_PWRGD only after VR
power good), follows SLP_S3/SLP_S5 down natively with stock delays, and
switches every output off in force_g3 order, latched, on 3VALW loss, VR
power-good timeout or loss, thermal halt, eSPI hang, a 1 s Python lease
expiry or a reload. `power.c` runs it from a 1 ms kernel timer;
`lilac.power_*` is the Python side. Images up to 8 were **disarmed**.

Image 9 (2026-11-15, flashed and run on Lilac; images 10-12 keep it): **armed**
(`LILAC_POWER_ARMED` 1, the header default; user decision with EC-telemetry-only
measurement, gate D/E option A in the playground plan). Assertions follow the
interlocks above. Also: the ISL9241 adapter current limits (0x3f, 0x3b) join
the charger write allowlist as whole fields bounded natively to 500..3000 mA
(ERANGE otherwise; the bring-up ceiling); the six stock ADC channels on their
stock pads, read-only, `lilac.adc_channels()/adc_read(name)` in mV (gain 1,
internal 2816 mV reference, 10 bit, as the stock shim); the 512-byte host
shared-memory window readable and masked-writable from Python
(`lilac.memmap_read/memmap_write`) except the native host-command flags byte
0x27 (EPERM; check in pure `hostif.c`).

Image 10 (2026-11-15, run on Lilac): allocation-light status calls for the S0 service
loop (`lilac.power_names/power_state/fan_state/port80_last`, small tuples
instead of string-keyed dicts), `lilac.heap_info()` (outer heap largest free
block), a 512-deep Port 80 ring that keeps the newest codes, and the fan
monitor treating the sensor rail as powered only after `slp_s3_l` has been
high 500 ms (an I2C bus error on the first read at S0 entry before).

Image 11 (2026-11-16, `f07c3b3a1b`): `supervisor/port.c` reports the real largest
free block of Zephyr's malloc (heap growth failed with a fixed 64 KB before).

Image 12 (2026-11-16, running on Lilac; BIOS setup visible on the eDP panel
in run 9): S0 peripherals as stock's chipset resume/suspend
hooks — `ec_bkoff_l` (GPC7, panel backlight enable, stock
`gpio_sm_panel_bken_ec`) is driven high while SYS_PWRGD is asserted and
`slp_s3_l` is high (`powerctl_s0_peripherals()`, applied from the 1 ms tick),
low otherwise; reported as `backlight` in `power_status()` and bit 16 of
`power_state()`'s inputs. `ec_mute_l` stays an input as in stock Lilac.

Raw GPIO/NVM modules and busio registration are excluded; UART internals remain
compiled for the native console. The board module contains inert pin names. No charger policy, PD,
keyboard controller, PROCHOT or watchdog services are implemented; cooling is verified only by the
native fan floor in G3. Powering the AP is limited to bounded, supervised bring-up runs.

Pin source: ec-lilac 6bcd05acb6c11cc2a2e247118b9e741bf7e6d1f0,
zephyr/program/framework/lilac/gpio.dtsi. All stock output defaults are
preserved, including 1.8 V RSMRST, 1.8 V open-drain PBTN low and released
PROCHOT. Pin voltages, external pulls, reset/ROM gap and supply arrangement
require electrical review and measurement. Python reset handling does not
cover the time before native main or replace native emergency protection.
User confirmation is required before the first hardware test.
