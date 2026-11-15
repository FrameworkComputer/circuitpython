# Framework NPCX993 UART bring-up

Experimental standalone CircuitPython target for the NPCX993FA0BX
(`npcx9m3f`, Cortex-M4F) used by the Framework EC targets in `../ec`.
This is a build-only bring-up; it has not been boot-tested on hardware.
It does not include the laptop EC application (power sequencing, charging,
fan control, keyboard scanning, or host commands), and is not an EC RO/RW
update image.

## Scope across Framework boards

This target is shared NPCX993 bring-up, not Sakura-specific. In the inspected
EC checkout, `zephyr/program/framework/BUILD.py` selects `npcx9/npcx9m3f`
for Lotus, Azalea, Marigold, Tulip, Sakura, Sunflower, and Dahlia. Its Kconfig
identifies all seven as NPCX993FA0BX, and their shared NPCX9 board definition
selects UART1 on GP64/GP65 at 115200 baud. None of those project overlays
overrides that UART routing.

These are candidate boards for this common image, not hardware-validated
CircuitPython boards. Connector access, UART voltage, programming/recovery,
and the effect of replacing the EC application must be checked per board.
The list describes this EC checkout, not every Framework product or revision.
For example, the older HX20/HX30 branch selects Microchip MEC152x and cannot
use this image.

Keep CPU, memory layout, boot header, and minimal runtime configuration common
here. When adding laptop peripherals, use separate board targets that include
shared NPCX993 definitions and supply their own GPIO names, bus pinmux,
attached devices, and startup behavior. In particular, laptop power-control
GPIOs must not be initialized by a generic shared profile. Other NPCX variants
need their own verified memory and boot configuration.

## Build

Initialize the Zephyr workspace and dependencies as described in the port
README, including `hal_nuvoton`, then run from `ports/zephyr-cp`:

```sh
make BOARD=framework_npcx993
```

The board uses an 8 KiB main stack, LTO, and `CIRCUITPY_FULL_BUILD = false`.
Optional modules, graphics, networking, Bluetooth, and USB are disabled.
The `busio` implementation requires Zephyr's I2C/SPI APIs even though only
UART1 is enabled in this target's devicetree. Dynamic bus pin routing on NPCX
is not implemented.

The first successful build used the manifest's Zephyr revision
`db2dd2aa0914b9b427baa6a3df3e6460699e4c80` and Zephyr SDK 1.0.1:

| Region | Used | Capacity |
| --- | ---: | ---: |
| Executable code RAM (linker `FLASH`) | 187,164 bytes | 262,144 bytes |
| Static data RAM, including stacks | 16,540 bytes | 65,536 bytes |

The remaining data RAM is available to the port heap; usable Python heap
must still be measured on hardware.

## Artifacts and layout

Artifacts are in `build-framework_npcx993/zephyr-cp/zephyr/`:

- `zephyr.elf`: symbols and executable for debugging.
- `zephyr.bin`: executable payload, linked at `0x10080000` in code RAM.
- `zephyr.npcx.bin`: payload with the 64-byte NPCX ROM boot header.
- `zephyr.npcx.hex`: Intel HEX representation of the header-bearing image.

The header-bearing image is intended for standalone ROM boot from flash
offset zero. Flash from `0x00000` through `0x40fff` is reserved for firmware
and its header. A separate FAT filesystem partition occupies `0x41000`
through `0x7ffff` (252 KiB, including the supervisor's scratch sector).
This layout replaces the EC firmware layout; do not use it with the EC's
normal RO/RW updater. No flash runner is configured. Programming and recovery
on the selected physical board must be established before flashing it.

## Console

UART1 is configured for **115200 baud, 8N1**, using the same pinmux as the
EC's `zephyr/boards/google/npcx9/npcx9.dtsi`:

- GP64: EC UART RX.
- GP65: EC UART TX.

Use the board-appropriate UART voltage and a common ground. On Linux, select
the adapter through `/dev/serial/by-id/`. There is no USB console or USB
mass-storage drive. The initial hardware check is the CircuitPython UART
REPL banner, followed by execution of a small script and a heap measurement.
