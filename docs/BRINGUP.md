# ECG Card hardware bring-up guide

Reader: the hardware designer, at a Linux laptop with a SEGGER J-Link on Tag-Connect J1, a
bench supply, scope and DMM. Board: untested. Firmware: has only run in simulation (host
tests and rendered screens). Everything below that the code or brief cannot prove is marked
**(unverified)**. Sources: `ecg-fw.md`, `README.md`, `scripts/`, `fw/bsp/README.md`,
`fw/app/`, `fw/acq/`, `fw/include/board.h`, `tools/README.md`, `small_oled_datasheet.pdf`.

Scope: the radio link (MSPM0 side) is implemented but has only run against a simulated
CH585; stage 17 covers it (unverified). With Bluetooth On (the default) `RADIO_EN` is high
whenever the card is awake. For the stages before 17, turn Bluetooth Off in Settings if no
radio firmware is flashed yet (otherwise the log shows `ble: radio FAULT` after 1 s; harmless,
the card works without the radio).

Test-point and net names: only TP8 (`VLED`), TP12/TP13 (`ISNS1`/`ISNS2`) and TP15
(`BODY_EN`) are named in the brief. For other nets probe the pull-down resistor pad from the
pin map: R13 `ANA_EN`, R42 `MEASURE_REQ`, R69 `FAST_RESTORE`, R5 `VLED_EN`, R6 `OLED_EN`,
R25 `BUCK_PS_SYNC`, R51 `GAIN_HI`, R48/R49 `RLD_L_EN`/`RLD_R_EN`; `LED_BLANK` = PA12 pin 27
(R45 pull-up, R44 to Q1 gate).

## 1. Before you start

### 1.1 Copy to the laptop

Copy the whole repository (without `build/` and `.venv/`), for example
`rsync -a --exclude build --exclude .venv ecg_card/ laptop:ecg_card/`.

`mspm0_sdk_2_11_00_07.run` is git-ignored (`.gitignore`: `mspm0_sdk_*.run`). Copy it by hand
into the repo root, or keep it anywhere and pass its path to `setup_toolchain.sh`.

### 1.2 Prerequisites

| Item | Why |
|---|---|
| `cmake` >= 3.20, `ninja`, `curl`, `tar`/`xz`, x86_64 Linux | `CMakePresets.json` (Ninja); toolchain download in `setup_toolchain.sh` |
| Network on first setup | `setup_toolchain.sh` downloads Arm GNU 14.2.rel1 with `curl` from developer.arm.com |
| SEGGER J-Link software: `JLinkExe`, `JLinkGDBServerCLExe`, and its library used through the Python package `pylink-square` (`.venv/bin/pip install -r tools/requirements.txt`) | `flash.sh`, `gdb.sh`, `where.sh`; `rtt.sh` (wraps `tools/rtt_read.py`). It must know device `MSPM0G3507` (unverified: if J-Link reports an unknown device, update the J-Link software) |
| `python3` with numpy, scipy, matplotlib | `tools/`. Optional venv below |
| `clang` | only for the host preset (`cmake --preset host`), not needed to build the target |

The scripts default to `~/ti/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi` (override
with `ARM_GCC_ROOT`) and `~/ti/mspm0_sdk_2_11_00_07` (override with `MSPM0_SDK_ROOT`).
Probe selection: `JLINK_SN`; speed `JLINK_SPEED` (default 4000 kHz); `JLINK_DEVICE`.

### 1.3 One-time setup and build

    cd ecg_card
    scripts/setup_toolchain.sh ./mspm0_sdk_2_11_00_07.run     # Arm GCC + TI SDK into ~/ti
    python3 -m venv --system-site-packages .venv && .venv/bin/pip install -r tools/requirements.txt   # optional
    cmake --preset target
    cmake --build --preset target

Outputs in `build/target/fw/bsp/`: `ecg_bsp_smoke.elf` (smoke image) and `ecg_fw.elf`
(application), each with `.hex`, `.bin`, `.map`. The build prints flash/RAM usage.

Compile-time options that matter at bring-up:

| Option | Where | Effect |
|---|---|---|
| `-DAPP_PPG_BLANK_DISPLAY=ON` on the `cmake --preset target` line | `fw/app/CMakeLists.txt` (default OFF) | blanks the OLED (`AEh`) while PPG is RUNNING (open question 10). Set back to OFF afterwards |
| `SSD1306_FLIP_180` | `fw/drivers/include/ssd1306.h`; no CMake option, add `#define` to `ssd1306.c` | rotates the image 180 degrees (`A0`/`C0` instead of `A1`/`C8`) |
| `-DBSP_OPA0_CHOP=<0 or 1>` on the `cmake --preset target` line | `fw/bsp/CMakeLists.txt` (cache option, default 1) | OPA0 chopping: 0 = off (`DL_OPA_CHOPPING_MODE_DISABLE`), 1 = standard. Any other value stops the configure. OPA1 chopping is always disabled (`hal_acq.c:655`) |

Build each chopping variant (same `build/target` directory; the cache keeps the last value, so set 1 again afterwards):

    cmake --preset target -DBSP_OPA0_CHOP=0 && cmake --build --preset target   # chopping off
    cmake --preset target -DBSP_OPA0_CHOP=1 && cmake --build --preset target   # standard (default)

`BSP_OPA0_CHOP` is the only `BSP_*` option. Flash after each build.

### 1.4 Flash and log (commands)

    scripts/flash.sh build/target/fw/bsp/ecg_bsp_smoke.elf      # smoke image
    scripts/flash.sh build/target/fw/bsp/ecg_fw.elf             # application
    scripts/rtt.sh -e build/target/fw/bsp/ecg_fw.elf log        # RTT channel 0 text -> terminal and build/rtt-log.txt
    scripts/rtt.sh -e build/target/fw/bsp/ecg_fw.elf both build/rtt.bin   # text + channel 1 binary stream -> build/rtt.bin
    scripts/gdb.sh build/target/fw/bsp/ecg_bsp_smoke.elf        # gdb, stops at main; RTT ch 0 on telnet localhost:19021

- With no argument `flash.sh`, `rtt.sh` and `gdb.sh` pick `ecg_fw.elf` if it exists, else the
  smoke image. **Always name the ELF you flashed**: `_SEGGER_RTT` sits at a different address
  in each image and `rtt.sh -e` reads it from the ELF.
- `flash.sh` erases only the sectors the image occupies; the settings sector (last 1 KB,
  0x1FC00) survives a reflash. It ends with reset and go.
- RTT channel 0 (1 KB) is text, channel 1 (2 KB) is the binary stream. Both are
  non-blocking and drop data when no host reads. Boot lines stay in the buffer until a host
  attaches, so starting `rtt.sh log` after `flash.sh` still shows the boot line.
- Running `flash.sh` while `rtt.sh` runs uses two J-Link connections at once (unverified;
  stop `rtt.sh` first if the probe complains).
- The stream tools (`tools/README.md`), run from any directory:

      python3 tools/stream_capture.py --follow build/rtt.bin --raw build/session.bin --npz build/rec.npz
      python3 tools/check_capture.py build/rtt.bin
      python3 tools/stream_plot.py --report build/rtt.bin --png build/report.png
      python3 tools/stream_plot.py --follow build/rtt.bin --window 8      # live plot, needs a display

  `check_capture.py` exits 0 only if there is no CRC/length/COBS error and ECG block starts
  step by 10, PPG block starts by 2 (a start index of 0 begins a new run). Options:
  `--min-ecg-blocks N --min-ppg-blocks N --require-events r_peak,pulse,lead,state,button`.
  `stream_capture.py` prints once per second: `frames`, `ecg`/`ppg` block counts, `bad_crc`,
  `seq_lost`, `fr/s`, `B/s`, HR.
- The smoke image writes raw 16-bit ECG codes to channel 1, not framed data: do not decode
  its channel 1 with the tools.

## 2. Safety, specific to this board

- **Bench supply first.** Until the charger stage, power the card from a current-limited
  bench supply at **3.7 V, 100 mA limit** into H1 in place of the cell (R93 is the battery
  link). A limit trip may show up on the next boot as `reset=4` (brown-out) on the boot line (unverified, BSP item 12).
- **Supply voltage matters to the firmware.** The battery policy refuses to start a
  measurement below 5 % and shuts down below 3 % or 3300 mV (`board.h`
  `BATT_WARN_PCT`, `BATT_CUTOFF_PCT`, `BATT_CUTOFF_MV`). With no cell profile loaded
  (`cw2015.c`) the percentage falls back to a voltage curve (5 % at about 3.54 V), so keep
  the supply at 3.6 V or higher except in the low-battery stage.
- **Do not plug USB with only the bench supply on H1.** The BQ24073 would try to charge
  into the supply output (supplies generally cannot sink current: my inference, not in the
  brief). Do the USB stages with the real cell.
- **Cell must have its own protection board.** U16 is not fitted and R60 bypasses it. The
  firmware cut-off (below 3 % or 3300 mV on two consecutive readings, polled every 10 s through the CW2015) is only a
  second line.
- **Laptop on battery (charger unplugged) whenever any electrode is touched.** The
  interlock only sees the card's own USB port, not a ground loop through the J-Link. My
  extrapolation (not in the brief): a mains-earthed scope ground clip or bench supply
  chassis is the same kind of loop; connect them only while nobody touches the electrodes,
  or use isolated/battery instruments.
- **A core halt during a measurement does not stop the LED timing, but the electrodes stay
  connected.** The timers use `DL_TIMER_CORE_HALT_FREE_RUN` (`hal_acq.c:175`): they keep
  running through a debugger halt, so `LED_BLANK` keeps pulsing and cannot freeze low.
  `MEASURE_REQ` stays as it was (TP15 stays high), the watchdog pauses on halt, and samples
  are lost while halted (overruns counted, `lost=` in the session summary). Prefer gdb with
  the smoke image or at the `main` breakpoint; halting mid-session just ruins that record.
- **LED hazard bound.** The firmware caps channel A at 15 mA per LED (`LED_MAX_MA`; the
  brief's 20 mA cap is not reachable on the internal 2.048 V reference).

What the firmware does (by code, simulation-tested only):

- Every enable defaults low (board pull-downs, then `hal_init()` drives all outputs low,
  `LED_BLANK` high).
- `rails_measure_req()` refuses to raise `MEASURE_REQ` while `CHG_PG` is low or `ANA_EN` is
  low; the `CHG_PG` interrupt drives `MEASURE_REQ` low itself; the app then runs the stop
  sequence (`rails_off()`, one function for every exit).
- 2 s window watchdog.

What it does not do:

- It cannot see VBUS above the 6.6 V charger overvoltage (brief, "Gap in coverage").
- It provides no isolation and no electrode current limit; that is hardware.
- Its battery cut-off needs a working gauge and a 10 s poll.
- Nothing has run on silicon.

**OLED supply: confirm before trusting the display at 3.3 V.** `OLED_VCC` is `+3V3`
switched by U8. `small_oled_datasheet.pdf` (Hansheng HS96L02, VER 1.0, `pdftotext -layout`):

| Datasheet item | Value quoted |
|---|---|
| 3.2 DC characteristics, "Supply Voltage for DC/DC", `VBAT`, internal DC/DC enabled | **min 3.5 V, max 4.2 V** (no typ) |
| Same, "Supply Voltage for Display" `VCC` generated by internal DC/DC | 7.0 to 7.5 V |
| `IBAT`, DC/DC enabled (VDD 2.8 V, VCC 7.25 V, 100 % on) | typ 25 mA, max 32 mA |
| "Supply Voltage for Logic" `VDD` | min 1.65, typ 2.8, max 3.3 V; `IDD` typ 160 uA, max 220 uA |
| `VCC` supplied externally (DC/DC disabled) | 8.5 to 9.5 V |
| 2. Absolute maximum, "Supply Voltage for Display" `VCC` | min 3 V, max 5 V |
| 2. Absolute maximum, logic pins SCL/SDA/RES/DC/CS | 1.65 to 3.3 V |
| Note 4 (lifetime, 10,000 h at 120 cd/m2) | stated at "VCC = 3.3 V, Ta = 25 C, 50% checkerboard" |

The module has seven pins (1.5): pin 2 is labelled `VCC` ("Power Supply for OLED"), and there
is no separate `VDD` pin. The electrical table names the DC/DC supply `VBAT`; I read pin 2 as
that supply, but the datasheet does not say so, and the extractable text does not say how the
logic supply is derived. The firmware turns the charge pump on (`8Dh, 14h` in
`ssd1306_power_on()`), i.e. the "internal DC/DC enabled" case. At 3.3 V the supply is inside
the 3 to 5 V absolute-maximum range and inside the one 3.3 V condition the datasheet quotes
(the lifetime note), but **0.2 V below the 3.5 V minimum of the DC/DC supply row**, and the
table lists no 3.3 V operating mode. No 3.3 V "logic supply mode" exists on this module.
Check: brightness and stability at 3.3 V across temperature and with `OLED_VCC` sagging
under load (25 mA typ). Datasheet page 8 shows the 4-wire SPI strap as R3 4.7K, R4 4.7K,
R8 marked "OE" (read as 0 ohm), R1/R2/R10 blank: confirm these on your module (brief step 5).
Datasheet 7.3 also recommends a 0.5 A fuse in the supply path.

## 3. Procedure

Each stage: Action, Look, Pass, Fail. Smoke image first (stages 0 to 4), then the
application. Stages 5 to 16 use `ecg_fw.elf`. The first lines of the log use `[<ms>] tag: text`
(`ms` = SysTick since reset).

### Hidden diagnostics pages (application)

Home, BUTTON2 (next) to the About page (HOME, ECG, Pulse, ECG+Pulse cards, Settings with its
6 items, About), then **hold both buttons 2 s on About** (`BTN_LONG_MS`). Footer shows
`Diagnostics n/6`. Refresh 250 ms.

| In diagnostics | Action |
|---|---|
| BUTTON2 short | next page (6 wraps to 1). Entering page 2 switches `ANA_EN` on |
| BUTTON1 short | back to About (page 2 first calls `rails_off()`); **on page 6: toggles radio program mode** |
| BUTTON2 long (2 s) | Home. BUTTON1 long = power off |

| Page | Lines (21 columns; exact text from `diag.c`) |
|---|---|
| 1 `PINS / SYSTEM` | `CHG_PG <0/1> STAT1 <0/1>`; `BTN1 up/down BTN2 up/down` (`!` appended on stuck button); `Charger NO_USB/CHARGING/COMPLETE/FAULT`; `Reset power-on/pin/WATCHDOG/software/brown-out/other`; `Up <s>s loop <max ms>ms` |
| 2 `ANALOG  ANA_EN on` | `settling...` for the first 300 ms, then `VMID <code> <mV>mV`, `(1650mV +-30)`, `ECG  <code> <mV>mV`, `TIA  <code> <mV>mV`, `PPGHI <code> <mV>mV`. mV = code x 0.8057 at the pin. DAC12 offset set for TIA 304 codes; `MEASURE_REQ`, `VLED_EN` stay low |
| 3 I2C | `I2C 60:ACK 62:ACK` (or `--`); `CW2015 ver 0x73 ok` (` ?` if not 0x73, ` FAIL` if gauge init failed); `VCELL <mV>mV` (refreshed every 10 s); `SOC <gauge>% use <shown>%` (`use --` while the gauge reading is not ready or invalid); `MCP4728 EEPROM set` (written this boot) / `EEPROM ok` / `FAIL`; `I2C errors <n>` |
| 4 `LAST SESSION` | `lost ECG <n> PPG <n>` (HAL overruns, last session); `drop <drops>/<frames>` (RTT stream frames not accepted / sent, since boot, counted even with no logger attached); `loop <ms>ms i2c err <n>` (longest main-loop gap while acquiring); `LED <code> DAC12 <code>`; `mid <code> 558x/1098x <50/60>Hz`. Shows `LAST SESSION: none` until a session has ended |
| 5 `FIRMWARE` | `ECG Card 0.1.0`, build date, build time, `Radio <fw> <name tail>` (or `n/a (no HELLO)`), `NV ok/blank/corrupt writes <n>` |
| 6 `RADIO <state>` | state `off`/`powering`/`retry`/`advertising`/`connected`/`shutdown`/`FAULT` (` (adv t/o)` after the 2 min advertising timeout); `CH585 <fw> <last 3 address bytes>` or `no HELLO yet`; `MTU <n> <interval>ms sub <hex>` when connected, else `no phone  pwr <power-ups>`; `rx <bytes> tx <bytes> ovr <RX overruns>`; `crc <n> bad <n> gap <n>` (frames from the radio); `drop <ours>/<radio's>` and `B1=prog`. **Program mode** (BUTTON1): `RADIO PROGRAM MODE ON`, `RADIO_EN` held high, the MCU sends nothing and waits for nothing, no display-off or standby; BUTTON1 again, BUTTON2 (next page), leaving diagnostics or power-off ends it with the normal shutdown |

### Log lines (RTT channel 0, application)

`boot: ECG Card 0.1.0 reset=<0 power-on, 1 pin, 2 watchdog, 3 software, 4 brown-out, 5 other>
mcp=ok|mcp=FAIL` plus ` (eeprom programmed)` on the boot that wrote the MCP4728 EEPROM;
`settings: blank: defaults | corrupt: defaults | saved | save FAILED`;
`batt: CW2015 not answering: battery unknown`; `session: start mode=<0 ECG, 1 Pulse, 2 both>
gain=558|gain=1098`; `session: vmid=<mV> mV mid=<code> tol=+-30` (ECG start, once); `session: blocked: USB power`; `session: refused: battery low`;
`session: refused: MCP4728 absent`; `session: start failed err=<1 VMID, 2 LED DAC, 3 USB,
4 start> vmid=<mV>`;
`ecg: <from> -> <to> (<why>)` on every ECG session state change (states `wait`, `settle`, `run`,
`hold` = settling again after running, `end`; why `stable`, `settled, beat`, `rail`,
`no stable signal`, `no beat`), `ppg: <from> -> <to>` in Pulse mode (`wait`, `settle`, `run`);
`session: end <user|abort|lead-off|usb|battery|timeout> run_ms= beats= hr= pulses= pat= lost=`;
`usb: plugged|removed`; `power: off requested | power-down screen | display off | standby | wake`;
`batt: critical mv= pct=`, `batt: low: measurement stopped`;
`fault: Button 1|2 stuck down: it is ignored`. The RLD mode is not logged.
Radio: `ble: radio power on`; `ble: HELLO fw <x.y.z> proto <n> addr <AA:..:FF> reset <n> name "ECG Card XXXX"`;
`radio: <text>` (every log line the CH585 sends); `ble: no HELLO: power-cycling the radio once`;
`ble: radio FAULT: <why>`; `ble: connected mtu <n> interval <ms> ms sub 0x<hex>`;
`ble: disconnected: advertising again`; `ble: cmd <n> -> <LINK_RES>`; `ble: shutdown (<why>)`,
`ble: no ACK to SHUTDOWN: power cut after 50 ms`, `ble: radio off`; `ble: program mode ON...|off`;
`time: set HH:MM (UTC+<min> min)`.

While a session is armed, one evidence line per second per signal (values of the last second
unless "so far"; under 110 characters, about 100 B/s each, so the 1 KB channel 0 buffer keeps
up even with a slow reader):

    [22644] ecg: run rail=0% min=1681 max=2490 avg=1904 fr=0 beats=1 hr=0 mains=61 lost=168 rs=0
    [20966] ppg: on led=1344 dac=1290 dk=304 on=1225 hi=1944/1869-2046 c=921 agc=0 dcc=1 p=21 pr=73 lost=0

`ecg:` state; `rail` % of the 4 kHz codes on a rail (< 205 or >= 3891); `min`/`max`/`avg` raw
codes (mid = the `session: vmid` code); `fr` FAST_RESTORE pulses; `beats` reported so far; `hr`;
`mains` 50/60 Hz residual uVpp (input-referred); `lost` samples re-filled so far (HAL overruns);
`rs` resyncs. `ppg:` `off`/`on` (finger, ECG+Pulse) or `off`/`set`/`run` (Pulse); `led` LED DAC
code; `dac` DAC12 offset code; `dk`/`on`/`hi` means of `tia_dark`, `tia_on`, `hi_on` with
`hi` min-max; `c` mean `tia_on - tia_dark`; `agc` LED steps and `dcc` DAC12 steps so far; `p`
pulses so far; `pr` pulse rate; `lost` frames re-filled so far.

### Stage 0: bare board (brief step 0)

- Action: unpowered. DMM continuity of every rail to GND; capacitance of left and right ring to GND.
- Pass: no rail shorted to GND (`VSYS`, `+3V3`, `+3V3_SW`, `+3V3A`, `OLED_VCC`, `RADIO_VCC`, `VLED`); left and right ring capacitance equal.
- Fail hardware: any short, then find it before power. Unequal ring capacitance: assembly or solder bridge on one electrode path.

### Stage 1: power path (brief step 1), MCU still blank

- Action: bench supply 3.7 V, 100 mA limit, into H1. J-Link not connected yet.
- Look: DMM on `VSYS`, `+3V3`, then `+3V3_SW`, `+3V3A`, `OLED_VCC`, `RADIO_VCC`, `VLED`. Supply current.
- Pass: `VSYS` about the supply voltage; `+3V3` = 3.30 V; all switched rails about 0 V. Record idle current (no target in the brief).
- Fail hardware: `+3V3` wrong or supply in current limit, check U3 (TPS63001) and the R93 link. A switched rail on with the MCU blank: its enable pull-down missing (R13 `ANA_EN`, R6 `OLED_EN`, R5 `VLED_EN`, R67 `RADIO_EN`) or switch leakage.

### Stage 2: flash the smoke image, RTT alive (brief step 2)

- Action: connect J-Link to J1 (card powered, pin 1 is a `+3V3` sense). `scripts/flash.sh build/target/fw/bsp/ecg_bsp_smoke.elf`, then `scripts/rtt.sh -e build/target/fw/bsp/ecg_bsp_smoke.elf log`.
- Look: log. First line `ecg_bsp_smoke: reset cause=<n> ` then one line per second:
  `t=<rtc s> ms=<SysTick ms> ev=<events this pass> btn1=<1 released/0 pressed> pg=<1 no USB/0 USB> ecg=<raw samples read> ecg_ovr=<n> ppg=<frames read> ppg_ovr=<n> ecg_idx=<timeline index of the next ECG sample> ppg_idx=<same for PPG> resync=<hal_acq_resyncs()> dma_pos_mismatch=<n> `.
  The smoke image never switches a rail by itself (only the commands in stage 3 do): expect all switched rails 0 V.
- Pass: flash completes; `t` and `ms` advance, `ms` about 1000 per line; `btn1=1`, `pg=1`; `ecg=0 ppg=0`, `resync=0`, `dma_pos_mismatch=0`; rails still 0 V; supply current stable.
- Fail hardware: J-Link cannot connect: check J1 pin 1 / SWDIO (PA19) / SWCLK (PA20) / NRST, `+3V3` present, `BSL_INVOKE` (PA18) low (R26; JP1 open).
- Fail firmware assumption: no RTT output: the J-Link did not find `_SEGGER_RTT` (BSP README item 16); you passed the wrong ELF to `rtt.sh -e`. No log and no `t` advance: hang in startup or `hal_init()` (clocks run at 32 MHz until acquisition); run `scripts/gdb.sh` and read where it stopped. `ms` far from 1000 per `t`: RTC runs on LFOSC (a few % accurate, no crystal), this is expected; large error: SYSOSC FCL internal-resistor mode (item 14).

### Stage 3: smoke, commands, timers and ADC counts

Command interface: poke `g_smoke_cmd` through gdb (unverified procedure):
`scripts/gdb.sh build/target/fw/bsp/ecg_bsp_smoke.elf`; at `main` type `continue`; to send a command press
Ctrl-C, `set var g_smoke_cmd = <n>`, `continue`. Read results with Ctrl-C, `print g_smoke_val`,
`continue`. Commands: 1 ADC one-shot of ECG/TIA/PPG_HI/VMID into `g_smoke_val[0..3]`; 2 CW2015 VERSION read
(`g_smoke_val[0]` = version, `[1]` = status 0 on success); 3 ECG acquisition at 80 MHz; 4 PPG+ECG with PPG
analog on; 5 stop; 6/7 clock fast/slow; 8 OLED bus test; 9 NV write test; 10 5 s timed STANDBY; 11 I2C recover; 12 reset.
Commands 3 and 4 raise `ANA_EN` themselves (then wait 10 ms and switch to 80 MHz; 4 also turns the PPG analog on with DAC12 offset
2048) and command 5 stops, turns the PPG analog off, returns to the slow clock and lowers `ANA_EN`; `MEASURE_REQ` and `VLED_EN` stay
low. Command 8 powers the panel (`OLED_EN` high, `OLED_RESET` held low), writes 64 bytes by blocking and by DMA SPI, then
detaches SPI and returns DC and CS low before `OLED_EN` falls. Command 9 writes a changed settings blob and then writes the
original back, so stored settings survive (`g_smoke_val[0]`, `[1]` = the two write statuses, 0 = OK). RTT channel 0 is served on telnet `localhost:19021` while gdb runs.

| Check | Action | Pass | If it fails |
|---|---|---|---|
| I2C and CW2015 | command 2 | `g_smoke_val[1] == 0`, `g_smoke_val[0] == 0x73` (115) | hardware: SDA/SCL pull-ups R64/R65 (4.7 kohm), CW2015 supply via R63 and cell/H1, bus shorts; scope SCL rise time (400 kHz Fast-mode allows 300 ns). Firmware: I2C polled driver (BSP item 10): `hal_bus.c` timeouts |
| ECG ADC count and ring | command 3, watch 30 s of lines | `ecg` rises 4000 per second measured against `ms` (delta ecg / delta ms = 4.000 +- 0.01); `ecg_ovr=0`; `ppg=0`; `ecg_idx` = `ecg` + `ecg_ovr`; `resync=0`. `dma_pos_mismatch` should stay 0: a non-zero value means DMASZ and DMADA disagree, i.e. one of them is not a live position register. The BSP uses DMASZ, and switches to DMADA by itself if DMASZ is seen stuck at its programmed value, so with `resync=0` and `ecg_idx` correct the ring still works: record the mismatch count (BSP item 3) | rate 1000 per second: ADC0 needs one trigger per averaged conversion (item 2). `ecg=0`: timer cross-trigger start failed (item 1) or ADC0/DMA setup. `ecg_ovr` growing about 8 per second or 4000 per second: DMA re-arm or MEMRES read-clear assumption (item 3) |
| PPG frames | send 5, then command 4 (it raises `ANA_EN` itself). Scope `LED_BLANK` (PA12) | `ppg` rises 100 per second, `ppg_ovr=0`; `ppg_idx` = `ppg` + `ppg_ovr`; `resync=0`; `LED_BLANK` period 10.000 ms (100 Hz) with about 200 us low pulse (details in stage 11) | `ppg` stuck 0 or low: ADC1 sequence re-arm (item 5) or `TIA_ADC_CHAN` (item 4). `LED_BLANK` never toggles or glitches low at start: item 7. Command 5 lowers `ANA_EN` again. (Unverified: the PPG analog drives PA22/PA16 into the `+3V3A` network, which is why `ANA_EN` goes on first.) |
| 80 MHz and SysTick | commands 3 then 6 | `ms` still about 1000 per `t`; no reset (`reset cause` line does not reappear) | PLL lock/FCC check (item 8): hang or reset. Ms rate off after the clock switch: SysTick reload |
| STANDBY | command 10, scope or DMM on supply | `ms` freezes about 5 s then continues; supply current drops. Target below 80 uA (brief) is judged in stage 14 | no wake or no log after wake: STANDBY0 wake path (item 9) |

Command 8 (OLED test) is safe on the first board: the module is powered (`OLED_EN` high, reset held low) before SPI is
attached, and every OLED pin is low again before `OLED_EN` falls. It shows nothing on the display (reset held low); the
application tests the display itself in stage 5.

### Stage 4: charger and interlock hardware (brief step 3), smoke image

- Action: remove the bench supply from H1, fit the protected cell (own protection board), with an ammeter in series with the cell lead. Plug USB-C from a power adapter (not the laptop). Read `btn1`/`pg` in the log.
- Look: log `pg=0` when plugged. `CHG_STAT1` (PA5) with the DMM or scope (not printed by the smoke image). DMM current into the cell; `VSYS`.
- Pass: `CHG_PG` low, `CHG_STAT1` low, about 270 mA into the cell, `VSYS` about 4.4 V (brief). Unplug: `pg=1`, `VSYS` back to cell voltage.
- Interlock static test, still the smoke image, USB plugged, nobody touching electrodes:
  `call hal_pin_write(PIN_ANA_EN, 1)`, then `call hal_pin_write(PIN_MEASURE_REQ, 1)`. Scope TP15 (`BODY_EN`). The firmware ISR acts only on a `CHG_PG` edge, so the pin stays high and the hardware AND is tested alone.
- Pass: **TP15 stays low** with `MEASURE_REQ` high and USB present (brief step 3 and Safety). Then clear both: `call hal_pin_write(PIN_MEASURE_REQ, 0)`, `call hal_pin_write(PIN_ANA_EN, 0)`. Do not unplug USB while `MEASURE_REQ` is high (TP15 would rise and connect the electrodes).
- Fail hardware: charge current 0 with `CHG_PG` low: input limit/ISET (3.3 kohm) straps, cell protection board open. TP15 high with USB present: U13 gate or `CHG_PG` connection: stop here, do not continue to electrode stages. `CHG_STAT1` toggling about 2 Hz: safety-timer fault.

### Stage 5: application, first boot, display, buttons (brief step 5)

- Action: `scripts/flash.sh build/target/fw/bsp/ecg_fw.elf`; `scripts/rtt.sh -e build/target/fw/bsp/ecg_fw.elf log`. Scope `OLED_EN` (R6 pad) and `OLED_VCC` at power-up; room dark, window visible.
- Look at the display: first the boot screen for about 2 s (`ECG Card`, a heartbeat trace that draws itself across in the first second, `Designed & built by`, the designer's name, `v0.1.0`; `build/app_shots/boot_splash.png`); the card boots underneath it (I2C, gauge, radio) and any button skips it to Home without acting. Then the Home screen: `ECG Card`, battery icon with `NN%`, `On battery`, bottom `hold: off` with a page dot row (`build/app_shots/boot_home.png`). Log first line: `[ms] boot: ECG Card 0.1.0 reset=0 mcp=ok` (+ ` (eeprom programmed)` on the very first boot) then `settings: blank: defaults` on a never-written board.
- Pass: image stable and upright; `OLED_EN` rises after `OLED_RESET` low, `OLED_VCC` about 3.3 V; reset held low >= 6 ms, then released; record the `OLED_VCC` current (datasheet `IBAT` typ 25 mA at full-on); no flicker for 60 s.
- Buttons: BUTTON2 steps HOME, ECG, Pulse, ECG+Pulse, Settings, About, HOME. Settings: BUTTON1 changes the highlighted item, BUTTON2 next item, after the last item BUTTON2 goes to About. Open diagnostics page 1 (see above): `BTN1`/`BTN2` toggle `up`/`down` while held (hold under 2 s: 2 s on BUTTON1 powers off). Pass: both buttons toggle; no `!`; log has no `fault: Button`. A button held at boot is ignored until released; 5 s continuous gives the log line `fault: Button 1 stuck down: it is ignored` and a Fault overlay. A button held more than `BTN_HELD_MAX_MS` (10 s) after boot is treated as stuck in the same way (overlay once, then ignored), so a long deliberate hold is not a hardware fault: release and press again.
- Settings persistence: change one setting, wait 3 s or leave the screen: log `settings: saved`; page 5 `NV ok writes 1` after a reset.
- Fail hardware: blank display: `OLED_VCC` up? strap R3/R4/R8 (module 4-wire SPI); `OLED_RESET` stuck low (R53 pull-up); upside down: `SSD1306_FLIP_180` (section 1.3). Buttons permanently `down`/stuck fault: TS-1075S pads 3 and 4 not joined internally (open question 1), check the C75/C76 10 nF, 20 ms software debounce is in `board.h` (`BTN_DEBOUNCE_MS`).
- Fail firmware assumption: garbage or partial image: init timing (`ssd1306_power_on()` waits 6 ms, then 3 ms after releasing reset, the datasheet sample waits 200 ms; try a longer delay), SPI mode 0 at 8 MHz, DMA flush (BSP item 13). Hang with no boot line: erased-flash read (item 11) or MCP4728 init loop. `NV corrupt/blank` after save: flash program (item 11), `hal_nv.c`.

### Stage 6: I2C devices, MCP4728 EEPROM, LEDs never glow (brief step 4)

- Action: diagnostics page 3. Optionally watch the PPG window (green LEDs) in the dark during the boot that programs the EEPROM.
- Look: `I2C 60:ACK 62:ACK`; `CW2015 ver 0x73 ok`; `VCELL`, `SOC`; `MCP4728 EEPROM set` on the first boot, `EEPROM ok` on later ones; `I2C errors 0`.
- The first I2C job of `app_init()` is `mcp4728_init()`: it reads back the 24 bytes, and unless every channel's EEPROM is VDD reference (VREF=0), PD=01 (1 kohm to ground), gain x1, code 0 on all four channels, it Sequential-Writes all four channels to that state (a part programmed by an earlier build with channel A on the internal reference is rewritten) and polls the RDY bit for up to 100 ms; then powers all channels down, again with VREF=VDD. The internal 2.048 V reference is selected only while channel A is on. A second reset must show no ` (eeprom programmed)`: the EEPROM is written once only.
- Pass: both ACK; version 0x73; MCP4728 `EEPROM set` then `ok`, `mcp=ok` in the boot log; `VCELL` equals the cell voltage at the board (DMM; suggested limit 50 mV); LEDs never glow, at any point; `GREEN_SET` is about 0 V (it is shorted by Q1 while `LED_BLANK` is high; the LED sinks and `VLED` are off anyway).
- Fail hardware: `--` at 60: MCP4728 supply (via R87), address pins, SDA/SCL. `FAIL`/`mcp=FAIL` after EEPROM write: bus noise during the EEPROM write; with `mcp=FAIL` the app refuses Pulse (`MCP4728 not answering: pulse disabled`) but ECG still works.
- Fail firmware: `ver 0x..` not 0x73 shows ` ?` (recorded, not fatal). `SOC` unrealistic (brief open question 6): no cell profile is loaded (`cw2015.c` HOOK, UFG never set); `batt_pct_combine()` uses the gauge only if within 30 points of the voltage-curve estimate. `I2C errors` counting: polled-transfer assumptions (BSP item 10); `hal_i2c_recover()` does 9 clocks.

### Stage 7: analog rail and VMID (brief step 6)

- Action: diagnostics page 2 (BUTTON2 from page 1). Probe `ANA_EN` (R13), `BUCK_PS_SYNC` (R25), `+3V3_SW`, `+3V3A`, `VMID` net on the scope; DMM on `+3V3A`, `VMID`.
- Look: `ANA_EN` and `BUCK_PS_SYNC` rise together; `+3V3A` follows through R43; after 300 ms the screen shows `VMID`, `ECG`, `TIA`, `PPGHI`.
- Pass: `+3V3A` about 3.28 V (brief); `VMID` net about 1.65 V; screen `VMID` code 2011 to 2085 (1650 +- 30 mV, same limit the ECG start uses: `ECG_VMID_TOL_MV`); VMID settles within about 300 ms (50 ms time constant). `TIA` about 304 (245 mV, the DAC8 bias) with the window dark; `PPGHI` near 2048 (about 1650 mV; DAC12 is set for that, the formula `PPG_HI = 11 x V_DAC12 - 10 x TIA_OUT`). `ECG` code: the brief expects near mid-scale; this page does not raise `MEASURE_REQ`, so the electrodes are disconnected: record what you see (the simulator shows it railed, 4004, which is a simulator assumption).
- **Risk (my calculation), now mitigated:** VMID is half of `+3V3A` (about 3.28 V) but the ADC reference is `+3V3` (3.30 V), so a healthy board reads about 1640 mV (code 2036), 10 mV under 1650. `ECG_VMID_TOL_MV` in `fw/include/board.h` is therefore 30 (its comment: the old +-10 left no margin). Record the real VMID (page 2, and the log line `session: vmid=<mV> mV mid=<code> tol=+-30` at every ECG start) and tighten the tolerance later. Failure shows in stage 9 as `session: start failed err=1 vmid=<mV>` and the screen text `VMID <mV> mV, needs 1650+-30. Check +3V3A, U7B`.
- Fail hardware: `+3V3A` low: R43 drop, U23 (`+3V3_SW`) not enabled; VMID off: U7B, the 100k/100k divider, 1 uF. TIA railed or far from 245 mV: OPA0/photodiode wiring. PPGHI railed: R41/R40/C69, OPA1.
- Fail firmware assumption: TIA constant/garbage: `TIA_ADC_CHAN` (PA22 = ADC1 ch 8, `hal_acq.c:75`, BSP item 4). TIA railed or dead with a good circuit: OPA0 PSEL=DAC8 with NSEL=IN0- never verified as a combination (BSP item 6, open question 3; fallback R74/R75/C68 on PA26).
- Leaving the page switches `ANA_EN` off (`rails_off()`).

### Stage 8: ECG sampling rate and overruns (stream capture)

- Action: Home, ECG card, BUTTON1 (Start). No thumbs needed: sampling runs in "Place both thumbs" state. Start the capture first: `scripts/rtt.sh -e build/target/fw/bsp/ecg_fw.elf both build/rtt.bin` (terminal 1), `python3 tools/stream_capture.py --follow build/rtt.bin --npz build/rec.npz` (terminal 2). Let it run 10 min, then Ctrl-C both. Scope `ANA_EN`/`MEASURE_REQ` at start.
- Start sequence you will see (from `fw/acq/src/acq.c`, `board.h`): t=0 `ANA_EN` and `BUCK_PS_SYNC` high; at 400 ms (`ACQ_VMID_WAIT_MS`) VMID checked; then `GAIN_HI`/`RLD_L_EN`/`RLD_R_EN` set per Settings and, in the same pass, `MEASURE_REQ` high (TP15 high if on battery) with ECG timers starting, so about 400 ms after the press. `FAST_RESTORE` stays low until `MEASURE_REQ` is high; then it gets a 30 ms pulse (`ECG_FAST_RESTORE_MS`) whenever the output has been on a rail for 50 ms (`ACQ_FR_RAIL_MS`), at most every 250 ms (`ACQ_FR_REPEAT_MS`), so with no thumbs (floating inputs on a rail) expect about 4 pulses per second (`fr=4` in the `ecg:` line). In range it gets one more pulse only 500 ms after the signal became stable (`ACQ_FR_RECHECK_MS`) if the baseline is still over 25 % of full scale from mid.
- Look: log `session: start mode=0 gain=558`; stream rate on `stream_capture.py` line: about 50 ECG blocks/s, about 2.8 kB/s (33 B per ECG block, 23 B per PPG block, plus events); `seq_lost=0`. After stopping (BUTTON1 on the live screen) the log has `session: end user ... lost=0`; diagnostics page 4 `lost ECG 0 PPG 0`, `drop 0/N`, `loop <ms>`.
- Pass (exactly 500 samples/s):
  1. `python3 tools/check_capture.py build/rtt.bin` exits 0: no CRC errors and every ECG block start steps by 10.
  2. Log `lost=0`. The app re-inserts lost samples to keep indices continuous, so check_capture alone cannot see overruns; `lost=` and page 4 can.
  3. Wall-clock rate: `python3 -c "import sys; sys.path.insert(0,'tools'); from ecgcard.record import Recording; r=Recording.load('build/rec.npz'); print(r.n_ecg, r.n_ppg)"`; `n_ecg` divided by the session length measured on the laptop (start the logger first, press Start, press Stop, Ctrl-C at once) must be 500 +- 1 over 10 min (a second of start/stop error is 0.17 %). The count comes from one 4 MHz tick (`TIMG6` LOAD 999), so it is only as exact as the clock (BSP item 14); the precise test is the 100.000 Hz `LED_BLANK` on a counter in stage 11.
  4. In a BOTH session, `n_ecg / n_ppg` = 5.000 exactly (common time base).
  5. Page 4: `drop 0/N` with the logger attached from boot, `loop` below 60 ms (the ECG ring holds at least 64 ms, `hal.h`).
- Fail: `drop` nonzero: RTT not read fast enough or logger not attached (frames sent with no reader are dropped and counted; reset the card and attach first). `seq_lost` > 0 with `drop 0`: capture side. `lost` > 0: overruns (BSP items 2, 3). `session: start failed err=1`: VMID (stage 7 risk). Hardware: no `MEASURE_REQ`/TP15 rise at start with USB absent: U13, `CHG_PG` high, `ANA_EN` (the U13 supply).

### Stage 9: ECG with a signal generator (brief step 7)

- Setup (brief): generator through a resistive divider into the two rings, right-leg drive to the divider midpoint. Amplitude 1 mV peak (2 mVpp) at 10 Hz is inside both input ranges (+-2.96 mV at 558x, +-1.50 mV at 1098x). Battery power, USB unplugged.
- Force gain: Home, BUTTON2 x4 to Settings (first item `Gain`), BUTTON1 cycles `Auto`, `558x`, `1098x`; BUTTON2 steps to the next item (`Mains notch`, `RLD`, `LED limit`, `Sweep`); leaving the screen saves (log `settings: saved`). Set notch to `50Hz` or `60Hz` and RLD `Both`. Start an ECG session; the log line `session: start mode=0 gain=558` or `gain=1098` is the firmware's selection; also check `GAIN_HI` (R51 pad) low/high. `Auto` uses 558x unless the last session advised the other.
- Capture 30 s per condition (`rtt.sh both`, `stream_capture.py --npz`). Gain from the capture:

      python3 - <<'EOF'
      import sys, numpy as np
      sys.path.insert(0, "tools")
      from ecgcard.record import Recording
      r = Recording.load("build/rec.npz")
      x = r.ecg_uv[2500:]                       # skip the first 5 s
      print("pp uV", np.nanmax(x) - np.nanmin(x), "rms uV", np.nanstd(x), "gain flag", set(r.ecg_gain))
      EOF

- Pass: p-p equals the applied input p-p within 3 % at both 558x (expect 1385 raw codes p-p at 2 mVpp) and 1098x (2726 codes). The tools scale with the flagged gain, so a scale error is a real gain error: true gain = nominal x (shown / applied). Repeat at 0.5 Hz and 150 Hz: about 0.707 of the 10 Hz amplitude (brief: -3 dB near 0.5 Hz and 150 Hz); the stream is the 500 Hz decimated data and the notch only affects the display. Shorted-input noise: short both rings through the divider, record `rms uV` for the record sheet (no pass number in the brief).
- Log: `acq: contact` about 20 ms after start (80 samples off the rail), `acq: running` 2.5 s later (`ECG_SETTLE_MS`).
- Fail hardware: output railed (no contact): right-leg drive reference missing, input common-mode out of range; gain wrong: R-values of the gain stages (0.1 % U6A resistors, R68 switched by U18 `GAIN_HI`); `GAIN_HI` pin not toggling: U18 or `+3V3A`. Firmware: auto lead-off thresholds are 5 %/95 % of full scale (codes 205/3891) for more than 100 ms; the 25 % baseline recheck.

### Stage 10: ECG with thumbs, RLD, restore, interlock (brief steps 8 and Safety)

- **First, USB-plug test with the generator setup, no thumbs** (laptop J-Link may stay; USB from an adapter): start ECG (TP15 high), plug USB. Pass: TP15 goes low at once (hardware AND with `CHG_PG`), log `usb: plugged` then `session: end usb ...` (summary only if the session had reached RUNNING), screen `Stopped / USB connected. ECG runs on battery only.`, `MEASURE_REQ` low. Then, with USB still in, press Start: log `session: blocked: USB power`, screen `Unplug USB to measure ECG`, TP15 and `MEASURE_REQ` stay low. Unplugging USB on that screen re-arms the session by itself.
- Thumbs: laptop on battery. Start ECG, both thumbs on the pads and hold still. Log `ecg: wait -> settle (stable)` (about 2 s after the touch with dry thumbs: the offset step decays through U5), then `ecg: settle -> run (settled, beat)` about 2.5 s later; the screen shows the trace, heart rate, quality. Moving a thumb while running gives `ecg: run -> hold (rail)` ("Hold still") and back to `run`; lifting the thumbs gives `ecg: hold -> end (no stable signal)` 2.5 s later and the summary. Read the `ecg:` lines: `rail`, `min`/`max` show how long the front end stays saturated after a touch, `mains` the contact quality.
- Pass: QRS visible, HR plausible (check by hand count or a reference); no oscillation on the right-leg drive (scope U6B output/thumb islands); compare the four RLD modes `Both`, `Left`, `Right`, `Off` (one session each, set in Settings; not in the log) using the mains line in `stream_plot.py --report` (spectrum with 50/60 Hz markers) and the quality number; `FAST_RESTORE` (R69 pad, 30 ms pulses) recovers the baseline within about 3 s after a thumb removed less than 2 s (longer ends the session: `session: end lead-off`, `CONTACT_LOST_END_MS` 2000); lead-off detected after more than 100 ms on a rail (brief: within 200 ms).
- Open question 9 (D8 drift): with a 5 min record compare slow baseline drift (`Recording` wander) with thumbs swapped; the stream has one lead, so left/right can only be compared by swapping.
- Fail: no contact with thumbs: electrode paths, U20/U21 closed? (`BODY_EN`), ring contact. Mains too high in `Both`: try `Left`/`Right` (brief right-leg drive table). Settling longer than 3 s: coupling time constant (0.5 s), `ECG_SETTLE_MS`.

### Stage 11: LED drive on the scope (brief step 9)

- Action: Pulse card, BUTTON1 (Start), no finger. LED limit setting any (the start current is `LED_START_MA` 5 mA, AGC acts only with a finger present). Scope `LED_BLANK` (PA12, trigger on its falling edge), TP12 and TP13 (`ISNS1`/`ISNS2`), TP8 (`VLED`).
- Start sequence (`acq.c`): t=0 `ANA_EN`, `BUCK_PS_SYNC`, PPG analog and `VLED_EN` high together; 10 ms later (`ACQ_VLED_WAIT_MS`) MCP4728 channel A written to 1344 (5 mA); then timers start.
- Expected waveform per 10 ms frame (`board.h`, `hal_acq.c`): `LED_BLANK` high; falls at 9.800 ms (`PPG_T_LED_ON_US`); rises at 9.99975 ms (one tick before the frame end); LEDs on about 200 us (2 %). Internal ADC1 triggers at 9.700 ms (dark) and 9.950 ms (on) are not visible; see `TIA_OUT` settling instead.
- Pass: `VLED` (TP8) about 4.99 V. `LED_BLANK` clean 3.3 V edges, period 10.000 ms (100.000 Hz on a counter). `ISNS1` = `ISNS2` pulse of 41 mV at code 1344 (5 mA x 8.2 ohm; formula `I = code x 3.718 uA`, `V = I x 8.2 ohm`); 82 mV at 10 mA, 123 mV at 15 mA. Never a DC level on `ISNS` between pulses. `TIA_OUT` settled at +150 us after the falling edge (the on sample), `PPG_HI` too (10 us time constants, 8 needed). `GREEN_SET` low outside the pulse.
- Fail hardware: `VLED` not 4.99 V: U4 (TPS61040), L/D; no boot start in 5 ms. No pulse: Q1/R44/R45, U11 sinks on `+3V3_SW`, MCP4728 reference. Pulse height wrong by a ratio: 154k/10k divider, 8.2 ohm sense resistors.
- Fail firmware: `LED_BLANK` low or glitching before the first frame, constant low (LEDs on continuously), or inverted: BSP item 7 (CCPIV/LACT/CDACT polarity). Pulse not 200 us: `PPG_T_LED_ON_US`. Height ratio off: constants `LED_UA_PER_CODE_X1000 = 3718`, internal 2.048 V reference x1 (`mcp4728.c` `put_block` writes VREF=1, G=0).

### Stage 12: PPG with finger and black tape (brief step 10)

- Action: Pulse card, Start; right thumb on the sensor. Capture with `rtt.sh both` + `stream_capture.py`.
- Look: log `acq: contact` (coarse above 250 codes for 300 ms, `DSP_FINGER_ON`), `acq: running` 2 s later; screen shows trace, pulse rate, `PI %`. Diagnostics page 4 after stopping: `LED <code>`; AGC target is TIA_OUT(on) 1.5 to 2.0 V (codes 1862 to 2482), steps +25 %/-20 % outside 0.6 to 2.8 V, capped by the LED limit (5/10/15 mA, codes 1344/2689/4034).
- After 5 s without a detected pulse (`ACQ_PULSE_TIMEOUT_MS`) the pulse rate, `PI` and PAT show `--` until pulses return (also in ECG+Pulse, stage 13): it is not a fault if the finger moved.
- Pass (finger): pulsatile signal, pulse rate within a few bpm of a reference, `PI` plausible, `lost PPG 0`, stable `LED` code, `PPGHI` in range (`dac12` adjusts about once per second).
- Pass (black tape over the window, no finger): the log never shows `acq: contact`; the stream PPG flag bit 2 (no finger) stays set; mean coarse (`r.ppg_coarse`, 0.81 nA per code) is small, well below 250 codes (about 200 nA). Record the mean as the optical leak number:

      python3 - <<'EOF'
      import sys, numpy as np
      sys.path.insert(0, "tools")
      from ecgcard.record import Recording
      r = Recording.load("build/rec.npz")
      print("coarse mean", np.nanmean(r.ppg_coarse), "led code", r.ppg_led_code[-1], "no-finger", r.ppg_no_finger.mean())
      EOF

- LED current vs DAC code: the capture records `led_code` per block; pair AGC-chosen codes (different fingers/targets) with the `ISNS` pulse height. The firmware offers no way to set an arbitrary code (gdb is not safe here).
- Fail hardware: finger never detected: photodiode polarity/bias, TIA feedback R73/C67, window; high leak with tape: optical path. Fail firmware: no pulse in a clear signal: DC cancel or foot detector (`dsp_ppg.h`); `PPG_HI` railed: DAC12 loop; noisy TIA: OPA0 chopping (`BSP_OPA0_CHOP` 0 vs 1, section 1.3) and OLED interference (stage 16).

### Stage 13: ECG + Pulse, PAT plausibility

- Action: ECG+Pulse card, Start; both thumbs on the pads, right thumb also on the PPG window. Capture 2 min.
- Look: log `session: start mode=2`; screen `PAT <ms>` and `Pulse`; `session: end ... pat=<ms>`; capture events `pulse` (field `pat_ms`).
- Pass: PAT values only appear when 100 <= PAT <= 500 ms (`dsp_pat.h` accepts only that range); beat-to-beat scatter small and PAT stable across the record; `n_ecg / n_ppg` = 5.000. A PAT outside the window is silently discarded: absent `PAT` means no pairing, not a zero.
- Fail: no PAT while both HR and pulse are good: timeline offset between ECG and PPG start (BSP item 1, cross-trigger start); `hal_acq.c` residual offsets are documented as under 1 tick (250 ns).

### Stage 14: power off, standby current, wake, timeouts (brief power table)

- Action: Home. Hold BUTTON1 2 s: `Power off` overlay with a progress bar from 300 ms, then `Release the button`; on release the power-down screen (designer credit, `Powering off...`, `build/app_shots/power_down.png`) for 1.5 s, buttons ignored, then standby. Log `power: off requested`, `power: power-down screen`, then `power: standby`. Disconnect the J-Link (unverified: a debugger may keep the debug domain awake) and measure the supply current in a series DMM/µA range, averaged over several seconds.
- Pass: current below 80 uA from the cell (brief: TPS63001 about 40 uA, CW2015 about 15 uA, charger leakage <= 6.5 uA); all switched rails 0 V; display dark. Note the CPU wakes every 1 s (RTC tick) and every 600 s for a battery check (`STANDBY_BATT_WAKE_S`, no log line), so average rather than peak.
- Wake: press BUTTON1 (the wake press itself does nothing): log `power: wake`, the boot screen for about 2 s, then Home. USB plug or unplug also wakes, straight to Home. Fail: no wake or dead display after wake: STANDBY0 wake path and SPI1 re-init (BSP items 9 and 14). Current high: a rail left on (`rails_off()` runs first), pins driven into an off rail, or the MCP4728 internal reference left on (about +45 uA, IPD_INT 45 uA typ; every powered-down channel must select VDD, `mcp4728.c`).
- Timeouts: leave the card idle on Home. Log `power: display off` after 60 s (`UI_DISPLAY_TIMEOUT_S`); `OLED_EN` falls after a 100 ms power-down sequence (SCK/MOSI/DC/CS/RESET low first). Any button then wakes the display without acting. `power: standby` 300 s after the last input (`UI_STANDBY_TIMEOUT_S`; a phone command also counts as input for this one). Measuring, an overlay or radio program mode suspends the timeouts. Before standby the radio gets `ble: shutdown (standby)` and is unpowered.

### Stage 15: low battery, if practical

- Action: remove the cell, use the bench supply into H1 (current limit 100 mA), at 3.6 V reduce in 20 mV steps with the card idle on Home, watching diagnostics page 3 `VCELL` against the DMM (the gauge value is refreshed every 10 s). After boot or wake the gauge reads VCELL 0 / SOC 0 until its first conversion; that reading is not used (page 3 `use --`) and is retried every 100 ms for up to 2 s (`BATT_SETTLE_MS`); after that VCELL != 0 with SOC > 100 falls back to the voltage estimate.
- Pass: below 5 % (about 3.54 V on the voltage curve, if the gauge is implausible; the gauge SOC is used when within 30 points): Start refuses (`session: refused: battery low`, screen `Battery low / Measuring is off.`); a running session ends with `batt: low: measurement stopped`. Below 3 % or 3300 mV: the first critical reading is re-read 1 s later (`BATT_CONFIRM_MS`); only two consecutive critical readings (`BATT_CRIT_READS`) give `batt: critical mv=... pct=...`, screen `Battery empty / Shutting down`, 3 s later `power: standby` (a single dip does not shut down). `CW_ALRT` (PA13) falls at about 5 % (ATHD set to `BATT_WARN_PCT`) and the battery read happens immediately.
- Fail: percentages inconsistent: no CW2015 profile (open question 6). Stay above 3.0 V on the supply.

### Stage 16: display noise on PPG (open question 10)

- Action: record PPG noise (rms of the fine signal or of `TIA_OUT` on the scope) with a fixed finger or a fixed reflector on three builds: default (display on); display off by the 60 s timeout is not possible mid-session, so build with `-DAPP_PPG_BLANK_DISPLAY=ON`; and with OPA0 chopping off (`-DBSP_OPA0_CHOP=0`, section 1.3). Keep the same LED current (limit 5 mA).
- Pass: no number in the brief; compare, and decide whether blanking the display during PPG is worth keeping.

### Stage 17: radio (CH585), unverified

Needs the CH585 firmware (MounRiver project under `CH585F/`), a WCH-LinkE on J3 set **not** to
supply 3.3 V (brief "Programming"), and the browser client `tools/ble_client.html`.

- Program: Settings `Bluetooth On`. Diagnostics page 6 (`RADIO`), BUTTON1: `RADIO PROGRAM MODE
  ON`, log `ble: program mode ON`. `RADIO_VCC` (DMM) 3.3 V. Flash the CH585 from MounRiver
  through the WCH-LinkE. The card does not sleep while the page shows program mode.
- Look: after the flash the CH585 restarts: log `ble: HELLO fw ... name "ECG Card XXXX"` and
  `radio: ...` lines (still in program mode), page 6 line 5 shows the radio version.
- BUTTON1 again: `ble: shutdown (program mode off)`, `ble: radio off`, then `ble: radio power on`,
  `ble: HELLO ...`; page 6 `RADIO advertising`; About shows `ECG Card XXXX` and the radio firmware.
  Fail: `ble: no HELLO: power-cycling` then `ble: radio FAULT`: no `RADIO_VCC`, radio firmware
  not running, or the UART (baud, pins PA10/PA11 vs CH585 PB12/PB13, CH585 UART1 remap).
- Advertising: a phone scanner (or the browser client) lists `ECG Card XXXX`. Connect with
  `tools/ble_client.html`: log `ble: connected ...`, Home shows the steady Bluetooth rune
  (blinking = advertising, rune with `!` = fault). Start ECG from the client: the card shows
  the ECG screen; data, events and the status arrive in the client. Page 6 `drop` and `crc`
  should stay 0 (`drop <ours>/<radio's>`).
- Scope (optional): `RADIO_EN` (R67 pad) rises before PA10 (pin 18) goes high, and PA10 is
  low before `RADIO_EN` falls (STANDBY, Bluetooth Off, 2 min advertising timeout).
- Not covered: the CH585's sleep / wake handshake (protocol 1 keeps it awake while powered)
  and BSL over the radio.

## 4. Firmware assumptions to verify on first hardware

Ordered by risk (from `fw/bsp/README.md`, with the code's own flags and my additions). "Item" is
the BSP README number.

| # | Assumption | Stage | Symptom if wrong |
|---|---|---|---|
| 1 | Timer start by cross trigger: TIMA0 CTTRIG loads and enables TIMA0 and TIMG6 together (item 1) | 3, 8, 11 | `ecg` stays 0, `LED_BLANK` never toggles, ECG:PPG ratio not 5:1 |
| 2 | ADC0 repeat-single + TRIGGER_NEXT + hardware average x4 gives one 4-conversion burst per trigger (item 2) | 3, 8 | 1000 samples/s instead of 4000 |
| 3 | ECG DMA ring: DMAEN re-arm at each lap, MEMRES read clears the flag, DMADA live (item 3) | 3, 8 | steady `ecg_ovr` growth, `lost ECG` > 0 |
| 4 | PA22 is ADC1 channel 8 (`TIA_ADC_CHAN`, item 4) | 7, 12 | TIA code garbage or constant |
| 5 | ADC1 non-repeating sequence re-armed by toggling ENC in the ISR (item 5) | 3, 12 | `ppg` below 100/s, `ppg_ovr` growing |
| 6 | OPA0 PSEL=DAC8 with NSEL=IN0- (PA27) (item 6, brief open question 3) | 7, 12, 16 | TIA railed/noisy, no photodiode bias |
| 7 | `LED_BLANK`: CCPIV high while TIMA0 is disabled, LACT/CDACT polarity, 200 us window (item 7) | 3, 11 | LEDs glow at start or constantly, wrong pulse width |
| 8 | 80 MHz SYSPLL, FCC check, ULPCLK/2, I2C TPR 9, SPI SCR 4, flash wait states (item 8) | 3, 5, 8 | hang/reset (`Reset WATCHDOG` on page 1), garbled display or I2C after a session |
| 9 | STANDBY0 wake by RTC/GPIO, SPI1 re-init, watchdog margin, current (item 9) | 3, 14 | no wake, dead display, current above 80 uA |
| 10 | I2C polled driver: TX_DONE/RX_DONE with repeated start, auto-STOP after NACK, zero-length probe, 9-clock recovery (item 10) | 3, 6 | missing ACKs, rising `I2C errors` |
| 11 | Flash NV: erase/program from RAM with IRQs masked; no ECC fault reading erased flash (item 11) | 5 | hang at boot, `save FAILED`, `NV corrupt` |
| 12 | VMID check limit 1650 +- 30 mV (`ECG_VMID_TOL_MV`) vs the real `+3V3A`/`+3V3` ratio (my calculation: about 1640 mV expected) | 7, 8 | `session: start failed err=1`, VMID outside 1620 to 1680 mV |
| 13 | MCP4728 EEPROM first-boot write and the factory EEPROM concern in `app.c` | 6 | `mcp=FAIL`, `EEPROM` never `ok`, LED glow |
| 14 | SYSOSC FCL in internal-resistor mode (item 14); LFOSC only a few % | 2, 8, 11 | sample rate and 10 ms frame off by more than a few ppm to percent |
| 15 | LED scaling: 2.048 V reference x1, 154k/10k divider, 8.2 ohm (`LED_UA_PER_CODE_X1000` 3718) | 11 | `ISNS` pulse height off the 41 mV at 5 mA |
| 16 | OLED init: 6 ms rail, 3 ms reset, charge pump `8Dh 14h`; 3.3 V supply (section 2) | 5 | blank, garbled, dim display |
| 17 | SPI1 TX-only: RX FIFO overrun does not stall DMA (item 13) | 5 | display freezes after first frame |
| 18 | Reset-cause mapping: cold power-up may read PORHWFAIL or BORSUPPLY (item 12) | 5 | page 1 `Reset brown-out` after power-on (cosmetic) |
| 19 | Buttons get the internal pull-up in addition to the board's (item 15) | 5 | none expected |
| 20 | J-Link finds `_SEGGER_RTT` (item 16) | 2 | no log or stream |
| 21 | CW2015: no battery profile loaded (`cw2015.c`, `cw2015.h`) | 6, 15 | SOC wrong; voltage-curve fallback used |
| 22 | Timing constants: 400 ms VMID wait, 10 ms VLED wait, 30 ms `FAST_RESTORE`, 150 us TIA settle, 2.5 s ECG settle, finger thresholds 250/150 codes, TIA bias `PPG_TIA_BIAS_CODE` 304 | 8, 11, 12 | start failures, PPG sample before settling, false finger |
| 23 | Debug: timers free-run through a core halt (`DL_TIMER_CORE_HALT_FREE_RUN`, `hal_acq.c:175`) | all | none expected on `LED_BLANK`; samples lost and `MEASURE_REQ` unchanged during a halt (section 2) |
| 24 | UART0 to the radio (item 17): 460800 from ULPCLK at 32 and 40 MHz, pin mux, interrupt-driven TX/RX, divider retime at session start/stop | 17 | no HELLO (fault), or `crc`/`bad` climbing on page 6 during sessions only (80 MHz divider) |

## 5. Measurements to record

| Quantity | Where | Value |
|---|---|---|
| `VSYS`, `+3V3`, `+3V3_SW`, `+3V3A`, `OLED_VCC`, `VLED` (TP8) | DMM, stages 1, 7, 11 | |
| Idle current, bench supply 3.7 V, MCU blank / smoke | stages 1, 2 | |
| `VMID_MON` code and mV (page 2); `VMID` net by DMM | stage 7 | |
| `ECG`, `TIA` (dark), `PPGHI` codes with `ANA_EN` on | page 2 | |
| Shorted-input ECG noise, rms and pp uV, at 558x and 1098x | stage 9 | |
| Measured gain at 558x / 1098x (shown/applied) | stage 9 | |
| Charge current, `VSYS` on USB | stage 4 | |
| Currents: standby, Home with display, ECG, Pulse, ECG+Pulse | DMM, stages 5 to 14 | |
| LED `ISNS` pulse height vs `led_code` (5/10/15 mA) | stage 11 | |
| Optical leak: mean coarse with black tape | stage 12 | |
| TIA dark level, mV (page 2, window covered) and noise | stages 7, 12 | |
| `LED_BLANK` period (counter) and low pulse width (scope) | stage 11 | |
| RTT stream `drop D/N`, `lost ECG`/`PPG`, `seq_lost`, `loop` ms | page 4, capture | |
| PPG noise: display on / `APP_PPG_BLANK_DISPLAY=ON` / OPA0 chopping off (`BSP_OPA0_CHOP=0`) | stage 16 | |
| PAT mean and scatter | stage 13 | |
| Time from `OLED_EN` rise to a stable image | stage 5 | |
| Current: radio advertising, connected idle, connected streaming ECG+Pulse | stage 17 | |

## 6. Open hardware questions that bring-up answers

| # | Question (brief) | Stage | Note |
|---|---|---|---|
| 1 | TS-1075S pads 3/4 joined? | 5 | buttons read `down` permanently if not |
| 2 | PA31 wake from SHUTDOWN? | not answered | the firmware only uses STANDBY0 and never SHUTDOWN |
| 3 | OPA0 non-inverting from COMP0 DAC8; OPA0 noise as TIA | 7, 12, 16 | TIA level and noise, `BSP_OPA0_CHOP` 0 vs 1 |
| 4 | BSL pins on PA10/PA11 | not answered | no BSL bridge in this build |
| 5 | CH585 `PB22` bootloader | not answered | programming uses 2-wire debug (stage 17) |
| 6 | CW2015 battery profile | 6, 15 | the driver loads none; observe SOC vs `VCELL` |
| 7 | CH585 crystal loads | 17 | advertising found, connection stable |
| 8 | UART baud error at 460800 | 17 | MSPM0 side −0.08 % (32 MHz) / +0.06 % (40 MHz ULPCLK); CH585 firmware reports 458823 (−0.43 %): 0.35 % / 0.49 % apart; measure both |
| 9 | D8 ESD diode drift | 10 | swap-thumbs drift comparison (one lead only) |
| 10 | OLED noise into the PPG | 16 | display on/off, plus the 3.3 V supply question in section 2 |
| 11 | Radio sleep while streaming | not answered | protocol 1: the radio stays awake while powered |
