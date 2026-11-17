# ECG Card

Firmware and tools for the ECG card: an MSPM0G3507 (ECG, pulse, OLED, power) plus a
CH585F BLE radio.

## Contents

| Path | What |
|---|---|
| `fw/` | MSPM0 firmware: `include/` contracts, `bsp/` (only code using TI headers), `drivers/`, `dsp/`, `acq/`, `ui/`, `app/` |
| `CH585F/` | Radio firmware (MounRiver project); see its README |
| `host/` | Host simulator, test harness, UI renderer, radio logic tests |
| `tools/` | Python stream decoder/plotter/filter design, `ble_client.html` (browser BLE client); see its README |
| `scripts/` | Toolchain setup, flash, RTT log, GDB, radio build |
| `docs/BRINGUP.md` | Bring-up stages, log formats, bench notes |
| `ecg-fw.md` | Firmware brief |

## Build

    scripts/setup_toolchain.sh                    # once: Arm GCC + TI SDK into ~/ti
    cmake --preset host && cmake --build --preset host && ctest --preset host
    cmake --preset target && cmake --build --preset target

Flash with a J-Link (unplug it before ECG measurements):

    scripts/flash.sh build/target/fw/bsp/ecg_fw.elf

Reworked front-end gains: add `-DECG_GAIN_LO=<n> -DECG_GAIN_HI=<n>` to the target configure.

Radio: `scripts/build_ch585.sh` (output in `build/ch585/`).

Python tools: `pip install -r tools/requirements.txt`, then `python -m pytest tools/tests -q`.
