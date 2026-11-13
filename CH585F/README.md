# CH585F: ECG Card radio firmware

BLE peripheral and UART bridge for the WCH CH585F on the ECG Card. The
contract it implements is `fw/include/link_fmt.h` (with `stream_fmt.h`);
byte-identical copies live in `APP/include/` and a host test checks them.

## Layout

| Folder | What | Origin |
| --- | --- | --- |
| `APP/` | application: `main.c`, `ble_app.c` (GAP/GATT, notification queue), `link_mgr.c` (link protocol), `uart_link.c` (UART1 driver), `radio_board.c` (pins, watchdog) | ours |
| `APP/rl_*.c`, `APP/include/rl_*.h` | portable logic with no WCH headers: COBS/CRC framing, frame parser, message builders/parsers, notification queue, log formatter, advertising data. Tested on the host by `host/radio_tests/` | ours |
| `Profile/` | GATT services: ECG Card (custom), Device Information, Battery, Heart Rate | ours, in the style of WCH's profiles |
| `HAL/` | `MCU.c`, `RTC.c`, `SLEEP.c` (+ `KEY.c`, `LED.c`, excluded from the build) | WCH EVT `BLE/HAL`, unmodified |
| `LIB/` | `libCH58xBLE.a`, `CH58xBLE_LIB.h`, `ble_task_scheduler.S` | WCH EVT `BLE/LIB`, unmodified |
| `StdPeriphDriver/`, `Startup/`, `Ld/`, `RVMSIS/` | from the MounRiver CH585F template | identical to WCH EVT `SRC/` |

BLE stack options (`CONFIG.h` overrides) are project defines, as in WCH's
examples (Project Properties > C/C++ Build > Settings > GNU RISC-V Cross C
Compiler > Preprocessor): `CLK_OSC32K=0`, `DCDC_ENABLE=TRUE`,
`HAL_SLEEP=FALSE`, `BLE_BUFF_MAX_LEN=251`, `BLE_BUFF_NUM=10`,
`BLE_TX_NUM_EVENT=4`, `BLE_MEMHEAP_SIZE=16384`. `DEBUG` is deliberately
not defined: UART1 is the link and there is no debug UART. Application
constants (version, advertising and connection timing, watchdog) are in
`APP/include/app_config.h`.

## Build and flash (MounRiver Studio)

1. File > Import > Existing Projects into Workspace (MRS 1.x), or File >
   Open Project / load `.project` (MRS 2.x); select this `CH585F` folder.
   The toolchain is GCC12 (`.cproject`: RISC-V Compiler = GCC12).
2. Build (hammer). Output: `obj/CH585F.hex` and `.elf`. Expect about
   157 KB flash and 44 KB RAM (static; the stack uses the rest of the 128 KB).
3. Program with a WCH-LinkE on J3 (TC2030 pad: `RADIO_VCC`, `CH1_SWDIO`
   PB14, `CH1_SWCLK` PB15, GND), 2-wire mode:
   - The radio must be powered **by the card**: the MSPM0 has to hold
     `RADIO_EN` high (the MSPM0 Diagnostics "program mode" being added does
     this). Set the WCH-LinkE **not** to supply 3.3 V/5 V; with U17 off its
     output-discharge pull-down would load the probe's supply.
   - In the download configuration choose the CH585 and leave the option
     bytes as below.

A command-line build that mirrors this project exists for machines without
the IDE: `scripts/build_ch585.sh` (outputs to `build/ch585/`). MounRiver
remains the build of record.

### Option bytes (development)

`CFG_DEBUG_EN` = 1, `CFG_ROM_READ` = 1 (keep 2-wire debug), `CFG_BOOT_EN` = 1
(keep the bootloader), `CFG_RESET_EN` = **0** always (it would make PB23 a
reset pin; PB23 is not connected).

## What it does at boot

1. DC-DC on, 32 MHz crystal load 12 pF (`HSECap_12p`, `RB_XT32M_C_LOAD` =
   0001b), system clock 62.4 MHz from the HSE PLL.
2. Pins: unused pins input pull-up, PB22 (`CH1_BOOT`) input pull-up,
   `RADIO_IRQ` (PB4) push-pull low; the crystal pins PA10/PA11 and the debug
   pins PB14/PB15 are left alone.
3. 32.768 kHz crystal load 12 pF (`LSECap_12p`, `RB_XT32K_C_LOAD` = 0000b),
   then the stack powers the external 32 kHz oscillator (`CLK_OSC32K=0`).
4. UART1 remapped (`RB_PIN_UART1`) to PB12 (RX, pull-up) / PB13 (TX,
   push-pull), 460800 8N1. With Fsys = 62.4 MHz the divider is 17, so the
   real rate is 458824 baud (-0.43 %).
5. BLE stack, HAL, GAP peripheral role, link manager, BLE task. Watchdog on
   (538 ms, fed every 100 ms; `APP_WATCHDOG 0` in `app_config.h` turns it
   off for single-stepping).
6. Sends `LINK_HELLO` (protocol 1, firmware 0.1.0, BLE address LSB first,
   reset reason = `R8_RESET_STATUS & RB_RESET_FLAG`: 0 software, 1 power-on,
   2 watchdog, 3 manual, 5 power-on reset during sleep) and a STREAM_LOG boot line. It does
   **not** advertise until the MCU sends `LINK_RC_ADV_START` (100 ms for
   30 s, then 1 s). Name "ECG Card XXXX" (last two address bytes).

## Seeing its log

The radio has no debug port. Its log lines are STREAM_LOG (0x30) frames on
the link; the MSPM0 prints them on RTT channel 0 prefixed `radio: `. Lines
to expect: `boot: ...` (version, reset reason, BLE library, Fsys, UART
divider and baud), `ble: started, bd_addr ... matches HELLO`, connect /
disconnect / parameter / MTU / PHY events, and every 10 s while connected
`ble: sent .. drop .. q .. busy ..`. Link error counters
(`link: rx .. crc .. cobs ..`, `link: uart ovr .. fe ..`) are logged every
10 s when one of them has changed.

## Only verifiable on hardware

- Both crystal trims: carrier frequency offset under ±10 ppm (32 MHz) and
  RTC drift against the MSPM0 (32 kHz). Both are start values.
- The 460800 baud link: -0.43 % here plus the MSPM0's own error; run a
  frame-error soak (the `link:` counters above should stay at 0).
- UART RX latency margin: 2-byte FIFO trigger leaves 6 bytes (130 us) for
  interrupt latency under BLE load; watch `uart ovr`.
- RF: range with a hand on the card, TX power 0 dBm, antenna match (C40/C47
  not fitted).
- Throughput at the 20 ms interval: one ECG (25 B) + one PPG (15 B)
  notification per interval plus events, `BLE_TX_NUM_EVENT=4`; watch `drop`
  and `busy` in the `ble:` line and `dropped` in LINK_CONN_STATE.
- That the phone accepts the 20 ms / latency 0 / 4 s request, data length
  251 and ATT MTU 247 (logged as `ble: params`, `ble: mtu`).
- Byte order of `GetMACAddress()` (checked by the `bd_addr ... HELLO` log
  line) and the advertised name.
- Just Works bonding and reconnection with a bonded phone (CCCDs restored).
- Watchdog behaviour while the debugger halts the core.
- `BLE_MEMHEAP_SIZE` 16 KB: if the stack refuses it, `CH58x_BLEInit()`
  stops in a `while(1)` and no HELLO is sent.
