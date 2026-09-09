# Zephyr

This is an initial port of CircuitPython onto Zephyr. We intend on migrating all
existing boards onto Zephyr. To start, we'll only support new boards. Existing
boards will be switched as the Zephyr port reaches parity with the existing
implementation.

## Getting Started

First, install Zephyr tools (see [Zephyr's Getting Started Guide](https://docs.zephyrproject.org/4.0.0/develop/getting_started/index.html)). (These are `fish` commands because that's what Scott uses.)


```sh
pip install west
west init -l zephyr-config
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
west sdk install
```

Now to build from `ports/zephyr-cp`:

```sh
make BOARD=nordic_nrf7002dk
```

This uses Zephyr's cmake to generate Makefiles that then delegate to
`tools/cpbuild/build_circuitpython.py` to build the CircuitPython bits in parallel.

## Native simulator build container

Building the native sim requires `libsdl2-dev:i386` and other 32bit dependencies that
can cause conflicts on 64bit systems resulting in the removal of 64bit versions of critical
software such as the display manager and network manager. A Containerfile and a few scripts
are provided to set up a container to make the native sim build inside without affecting the
host system.

The container automatically mounts this instance of the circuitpython repo inside at
`/home/dev/circuitpython`. Changes made in the repo inside the container and on the host PC
will sync automatically between host and container.

To use the container file:

1. Build the container with `podman build -t zephyr-cp-dev -f native_sim_build_Containerfile .`
2. Run/Start the container by running `./native_sim_build_run_container.sh` on the host PC.
   The script will automatically run or start based on whether the container has been run before.
3. Init requirements inside the container with `./native_sim_build_init_container.sh`

To delete the container and cleanup associated files:
```sh
podman ps -a --filter ancestor=zephyr-cp-dev -q | xargs -r podman rm -f
podman rmi zephyr-cp-dev
podman image prune -f
podman rm -f zcp
```

## Running the native simulator

From `ports/zephyr-cp`, run:

```sh
make run-sim
```

`run-sim` starts the native simulator in realtime.
It prints the PTY path to connect to the simulator REPL.
If a local `./CIRCUITPY/` folder exists, its files are used as the simulator's CIRCUITPY drive.

Edit files in `./CIRCUITPY` (for example `code.py`) and rerun `make run-sim` to test changes.

## Shields

Board defaults can be set in `boards/<vendor>/<board>/circuitpython.toml`:

```toml
SHIELDS = ["shield1", "shield2"]
```

For example, `boards/renesas/ek_ra8d1/circuitpython.toml` enables:

```toml
SHIELDS = ["rtkmipilcdb00000be"]
```

You can override shield selection from the command line:

```sh
# Single shield
make BOARD=renesas_ek_ra8d1 SHIELD=rtkmipilcdb00000be

# Multiple shields (comma, semicolon, or space separated)
make BOARD=my_vendor_my_board SHIELDS="shield1,shield2"
```

Behavior and precedence:

- If `SHIELD` or `SHIELDS` is explicitly provided, it overrides board defaults.
- If neither is provided, defaults from `circuitpython.toml` are used.
- Use `SHIELD=` (empty) to disable a board default shield for one build.

## Naming extra pins

`board` names pins it can work out from the devicetree: LEDs, buttons and
recognised connectors. Anything a board merely routes somewhere has no name, so
name it in the [`/zephyr,user`][zuser] node, in a board overlay:

```dts
/ {
	zephyr,user {
		backlight-gpios = <&gpio3 11 GPIO_ACTIVE_HIGH>;
		ksi-gpios = <&gpio0 0 GPIO_ACTIVE_HIGH>, <&gpio0 1 GPIO_ACTIVE_HIGH>;
	};
};
```

A property with one entry names that pin after the property, so the first is
`board.BACKLIGHT`. A property with several appends the index, so the second is
`board.KSI0` and `board.KSI1` -- which keeps a wide keyboard matrix from needing
one property per pin. Only the controller and pin are read; how a pin is driven
is up to the Python code.

[zuser]: https://docs.zephyrproject.org/latest/build/dts/zephyr-user-node.html

## Framework Daisy

The Daisy keyboard and its dongle are nRF54LM20A boards whose Zephyr
definitions live in a separate module, [framework-daisy-zephyr][daisy], which
`zephyr-config/west.yml` pulls in -- so `west update` is all the setup there is:

```sh
make BOARD=framework_daisy_kb_dvt1
make BOARD=framework_daisy_dongle_dvt1
```

Unlike the other boards here these two build MCUboot as well, which their
`sysbuild.conf` opts into. Daisy ships with MCUboot in RRAM and the application
is linked into its slot, so an unsigned image would not boot. Two artifacts come
out of a build:

- `build-<board>/firmware.signed.bin`, which installs over the bootloader
  already on the hardware -- hold the pairing button (keyboard) or the button
  (dongle) at reset to reach its USB serial recovery. No debugger needed.
- `build-<board>/firmware.merged.hex`, bootloader and application together, for
  `make BOARD=... flash` over SWD.

The CIRCUITPY drive is 612 KB taken out of the application slot, because Daisy's
flash layout -- fixed by the bootloader already on shipping hardware -- otherwise
fills RRAM exactly. Reinstalling firmware over serial recovery therefore erases
the drive: MCUboot erases the slot as *it* understands it, which is the full
1828 KB. The reasoning and the numbers are in
`boards/framework/daisy_kb_dvt1/board.overlay`.

This port has no `pwmio` yet, so the keyboard overlay disables the PWM instances
and hands their pins back to `digitalio`: `board.WHITE_LED_1` through `_4`,
`board.RED_LED` / `GREEN_LED` / `BLUE_LED` and `board.BACKLIGHT`. The key matrix
is `board.KSI0`-`KSI7` and `board.KSO0`-`KSO17`; it is col2row and has no diodes,
so three keys at the corners of a rectangle read as a fourth and scanning it in
Python needs the same deghosting ZMK does. Of the three I2C buses `board.I2C` is
the pin header, `board.TP_I2C` the touchpad -- already powered, the board brings
its rail up at boot -- and `board.NPMC_I2C` the nPM1300, which is also the PMIC
supplying the rail the SoC runs on.

[daisy]: https://github.com/FrameworkComputer/framework-daisy-zephyr

## Testing other boards

[Any Zephyr board](https://docs.zephyrproject.org/latest/boards/index.html#) can
be used with CircuitPython. To test a different board, use `west` directly to
build the board. The build will do its best to support as much as possible. By
default the Zephyr console will be used for output. USB support is limited by
initialization support in `supervisor/usb.c`. Only flash regions not used by
Zephyr are used for CIRCUITPY. A manual `circuitpython` partition can be
specified instead.

For example, to test the `nrf52840dk` board:

```sh
west build -b nrf52840dk/nrf52840
```

This is already supported in `ports/nordic` as `pca10056`.
