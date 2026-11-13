# ECG Card firmware

MSPM0G3507 application firmware for the ECG card. Hardware reference: `ecg-fw.md`
(firmware brief) and `small_oled_datasheet.pdf` (SSD1306 module).

## Layout

| Path | Contents | Depends on |
|---|---|---|
| `fw/include/` | Contracts: `board.h`, `hal.h`, `settings.h`, `ui_model.h`, `stream_fmt.h` | — |
| `fw/bsp/` | `hal.h` on the MSPM0 (TI DriverLib), startup, linker, firmware executable | TI SDK |
| `fw/drivers/` | SSD1306, MCP4728, CW2015, charger decode, buttons | `hal.h` |
| `fw/dsp/` | Fixed-point signal processing, pure C | nothing |
| `fw/acq/` | ECG / PPG sequencers and control loops | hal, drivers, dsp |
| `fw/ui/` | Graphics, fonts, screens: `ui_model.h` -> frame buffer | nothing |
| `fw/app/` | State machine, navigation, power policy, settings, stream | everything |
| `host/sim/` | `hal.h` on the host, with rail-invariant checking | — |
| `host/testlib/` | `test.h` harness, `ecg_add_test()` | — |
| `tools/` | Python: filter design, reference models, stream decoder, plotter | — |

## Conventions

- C11, `-Wall -Wextra -Werror`. No floating point and no `malloc` in `fw/`
  (Cortex-M0+, 32 KB RAM, 128 KB flash). Host tests may use `double`.
- Only `fw/bsp/` includes TI headers. Everything else talks to `hal.h`.
- Each module directory owns its `CMakeLists.txt`: a static library
  `ecg_<module>` linking `ecg_iface`, plus tests under `if(NOT ECG_TARGET)`
  registered with `ecg_add_test(name SOURCES ... LIBS ...)`.
- Tests run from the repository root (`ctest --preset host`).

## Build

    scripts/setup_toolchain.sh            # once: Arm GCC + TI SDK into ~/ti
    cmake --preset host && cmake --build --preset host && ctest --preset host
    cmake --preset target && cmake --build --preset target
