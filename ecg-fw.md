# ECG Card Firmware Brief

Nov 2, 2026 · @Matt

## Overview

The TI MSPM0G3507 runs the whole card: acquisition, power, charging status, display and buttons. The WCH CH585 is a BLE radio that the MSPM0 powers on only when needed, and the two talk over one UART.

&#91;embedded content: system architecture · 2 processors, 7 blocks\]

Each line is a wired interface. The radio, display and analog front ends get power only while the MSPM0 enables them through the rail switches.

**What the card does**

- Single-lead ECG between the two thumb electrodes.
- Reflective green PPG under the right thumb: two LEDs and a photodiode.
- 0.96" OLED (128 × 64, SPI) and two buttons.
- 500 mAh LiPo, charged from USB-C.
- Streams ECG and PPG to a phone over BLE.

**Firmware deliverables**

1. **MSPM0 application:** acquisition, DSP, UI, power management, UART link master.
2. **CH585 radio firmware:** BLE peripheral, UART bridge, OTA.
3. **Bring-up and production test firmware:** one image per processor that exercises each block on command.
4. **Host tools:** a UART frame decoder and plotter, and a phone or desktop BLE test client.

**Five rules the design depends on**

- **One hardware timer owns all sampling.** ADC triggers and LED edges come from timer events, so interrupts, the display and the radio cannot add jitter.
- **The radio never blocks measurement.** The MSPM0 queues frames and moves on; dropped radio frames are counted, not waited for.
- **ECG only runs on battery.** A hardware AND gate disconnects the electrodes whenever USB power is present. Firmware should report that state, not try to work around it.
- **Everything defaults off.** Every rail enable and analog switch has a pull-down, so a reset or a crash leaves the board safe and quiet.
- **No pin drives an unpowered chip.** Before a rail turns off, every signal into it is driven low or made an input (section 8).

Unless stated otherwise, reference designators, values and pin numbers below are from the final schematic and PCB (2 Nov 2026). Values marked *estimate* still need measuring at bring-up.

## Hardware the firmware controls

Firmware gates five switched rails and owns every analog switch. Only `+3V3` and `VSYS` stay on permanently.

### Power tree

| Rail | Source | Enable (MSPM0 pin) | Feeds | Notes |
| --- | --- | --- | --- | --- |
| `VSYS` | BQ24073 (U2) OUT | always on | TPS63001, TPS61040 | 4.4 V regulated with USB; battery voltage without |
| `+3V3` | TPS63001 (U3) buck-boost | always on | MSPM0, MCP4728 (via R87), I²C pull-ups, buttons | `BUCK_PS_SYNC` (PA4): high = forced PWM (quiet), low = power save |
| `+3V3_SW` | TPS22918 (U23) | `ANA_EN` (PA8) | LED current sinks (U11) |  |
| `+3V3A` | `+3V3_SW` through R43 (10 Ω) | `ANA_EN` (PA8) | ECG front end (U5–U7), analog switches U18–U22, U25, U26, interlock gate U13 | Needed for ECG **and** PPG |
| `OLED_VCC` | TPS22918 (U8) | `OLED_EN` (PB2) | OLED module | Pulls up `OLED_CS` and `OLED_RESET` (R52, R53) |
| `RADIO_VCC` | TPS22918 (U17) + FB1 | `RADIO_EN` (PB18) | CH585 | Radio is fully off when low |
| `VLED` | TPS61040 (U4) boost, 4.99 V | `VLED_EN` (PB3) | LED anodes | On only during PPG |

The CW2015 fuel gauge (U14) runs straight from the cell through R63 and is never switched. It sleeps at under 1 µA.

### Charger

The BQ24073 (U2) is strapped for a 500 mA input limit, 270 mA charge current (ISET 3.3 kΩ) and the default 30 min pre-charge / 5 h safety timers. The NTC input is disabled. It needs no firmware to charge.

| PGOOD → `CHG_PG` (PA28) | CHG → `CHG_STAT1` (PA5) | Meaning |
| --- | --- | --- |
| high | high | No USB power |
| low | low | Charging |
| low | high | Charge complete (or charger disabled) |
| low | toggles at \~2 Hz | Safety-timer fault |

### Safety interlock

`BODY_EN` = `MEASURE_REQ` (PA3) AND `CHG_PG` (PA28). U13 computes it in hardware.

- **What it switches:** `BODY_EN` closes the two input switches (U20, U21) and the right-leg-drive master switch (U22).
- **USB present:** `CHG_PG` is low, so the electrodes stay disconnected whatever firmware does.
- **`ANA_EN` low:** U13 is powered from `+3V3A`, so `BODY_EN` is also low.
- **Gap in coverage:** PGOOD also reports "no input" when VBUS is above the charger's 6.6 V overvoltage threshold. Firmware has no VBUS measurement, so it can't close this gap. It only matters if a faulty charger puts out more than 6.6 V. The optional VBUS-detect FET from the charger review would close it in a later revision.

### ECG chain

The gain is 558× (`GAIN_HI` low) or 1098× (`GAIN_HI` high). The band is about 0.5–150 Hz, and the output is centred on `VMID` (1.65 V).

1. **Electrodes:** each through 100 kΩ and 10 pF to ground, with 100 pF across the pair. U20 and U21 connect them when `BODY_EN` is high.
2. **U5, instrumentation front end:** gain 21. AC-coupled through R18 and a 50 µF back-to-back tantalum pair, so the 0.32 Hz corner has a 0.5 s time constant.
3. **U6A, difference amplifier:** ×1 with 0.1 % resistors. This stage sets the common-mode rejection.
4. **C34 / R36, high-pass at 0.48 Hz:** U19 (`FAST_RESTORE`, PA2) shorts the node to `VMID` to recover after saturation.
5. **U7A, second stage:** gain 26.6, or 52.3 when U18 (`GAIN_HI`, PA9) switches in R68. Its 159 Hz low-pass is R33 with C33.
6. **R34 / C36, 339 Hz anti-alias:** feeds `ECG_ADC` (PA14).

- **Right-leg drive:** U6B integrates the common-mode signal and drives each thumb's island through U22 → U25 (`RLD_L_EN`, PB19) or U26 (`RLD_R_EN`, PB20) and 200 kΩ.
- **`VMID`:** U7B buffers a 100 k / 100 k divider filtered by 1 µF (50 ms time constant). `VMID_MON` (PB17) reads it through 1 kΩ.

### PPG chain

- **LED current:** MCP4728 channel A (I²C 0x60) feeds `GREEN_SET` through a 154 k / 10 k divider. Two TLV9002 current sinks (U11, Q2, Q3) then run each LED at `GREEN_SET` / 8.2 Ω.
  - Full scale is 24.5 mA per LED with the DAC on its VDD reference, or 15.2 mA on its internal 2.048 V reference.
- **Blanking:** `LED_BLANK` (PA12) drives Q1 through R44, and Q1 shorts `GREEN_SET`. High or floating means LEDs off, because R45 pulls the gate up. Low means LEDs follow the DAC.
- **Front end:** the photodiode (U24, anode to ground) feeds OPA0, set up as a transimpedance amplifier.
  - Feedback is R73 1 MΩ ∥ C67 10 pF (1 V/µA, 16 kHz). Output: `TIA_OUT`, PA22.
  - OPA1 follows as an inverting ×10 stage with a DAC12 offset (R41 / R40, C69). Output: `PPG_HI`, PA16.

## MSPM0G3507 configuration

Build the configuration in SysConfig from the table below, and let it flag pin-mux conflicts before writing any driver code. The board has no crystals on the MSPM0: the LFXT and HFXT pins are used as GPIO.

### Pin map (LQFP-48)

| Pin | Port | Mode | Net | Notes |
| --- | --- | --- | --- | --- |
| 1 | PA0 | I2C0\_SDA | `I2C_SDA` | 4.7 kΩ pull-up (R64). MCP4728 at 0x60, CW2015 at 0x62 |
| 2 | PA1 | I2C0\_SCL | `I2C_SCL` | 4.7 kΩ pull-up (R65) |
| 3 | PA28 | GPIO in, both-edge IRQ, wake | `CHG_PG` | Low = USB power present. 100 kΩ pull-up (R8) |
| 4 | NRST | reset | `TI_NRST` | 47 kΩ + 10 nF |
| 5 | PA31 | GPIO in, falling IRQ, wake | `BUTTON1` | Active low, 10 nF debounce (C75) |
| 8 | PA2 | GPIO out | `FAST_RESTORE` | High = short the ECG high-pass to `VMID`. Pull-down R69. Not the ROSC pin here |
| 9 | PA3 | GPIO out | `MEASURE_REQ` | High = request electrode connection. Pull-down R42 |
| 10 | PA4 | GPIO out | `BUCK_PS_SYNC` | High = TPS63001 forced PWM. Pull-down R25 |
| 11 | PA5 | GPIO in, both-edge IRQ | `CHG_STAT1` | BQ24073 CHG: low = charging. 100 kΩ pull-up (R9) |
| 14 | PB2 | GPIO out | `OLED_EN` | Pull-down R6 |
| 15 | PB3 | GPIO out | `VLED_EN` | Pull-down R5 |
| 16 | PA8 | GPIO out | `ANA_EN` | Pull-down R13 |
| 17 | PA9 | GPIO out | `GAIN_HI` | High = 1098×. Pull-down R51 |
| 18 | PA10 | UART0\_TX | `CH1_RXD1` | To CH585 PB12 through 1 kΩ (R58) |
| 19 | PA11 | UART0\_RX | `CH1_TXD1` | From CH585 PB13 through 1 kΩ (R57) |
| 20 | PB6 | GPIO out | `OLED_DC` | 1 kΩ series (R90) |
| 21 | PB7 | GPIO out | `OLED_CS` | Software chip select, active low. 1 kΩ series, pulled up to `OLED_VCC` |
| 22 | PB8 | SPI1\_PICO | `OLED_MOSI` | 33 Ω series |
| 23 | PB9 | SPI1\_SCK | `OLED_SCK` | 33 Ω series |
| 24 | PB14 | GPIO out | `OLED_RESET` | Active low. 1 kΩ series, pulled up to `OLED_VCC` |
| 25 | PB15 | GPIO in, falling IRQ, wake | `BUTTON2` | Active low, 10 nF debounce (C76) |
| 26 | PB16 | GPIO in, rising IRQ, wake | `RADIO_IRQ` | From CH585 PB4. **Enable the internal pull-down**: the line floats while the radio is off |
| 27 | PA12 | TIMA0\_C3 | `LED_BLANK` | High or floating = LEDs off. Low draws 33 µA through R45, so keep it high when idle |
| 28 | PA13 | GPIO in, falling IRQ | `CW_ALRT` | CW2015 low-charge alert, 4.7 kΩ pull-up |
| 29 | PA14 | ADC0 channel 12 | `ECG_ADC` | Analog |
| 31 | PA16 | OPA1\_OUT, ADC1 channel 1 | `PPG_HI` | Analog |
| 32 | PA17 | OPA1\_IN1− | `PPG_DCC` | Analog |
| 33 | PA18 | GPIO in, never driven | `BSL_INVOKE` | ROM bootloader starts if high at reset. Pull-down R26; JP1 bridges to `+3V3` |
| 34 | PA19 | SWDIO | `TI_SWDIO` | Tag-Connect J1 |
| 35 | PA20 | SWCLK | `TI_SWCLK` | Tag-Connect J1 |
| 36 | PB17 | ADC1 channel 4 | `VMID_MON` | 1 kΩ series |
| 37 | PB18 | GPIO out | `RADIO_EN` | Pull-down R67 |
| 38 | PB19 | GPIO out | `RLD_L_EN` | Pull-down R48 |
| 40 | PA22 | OPA0\_OUT, ADC1 channel 8 | `TIA_OUT` | Analog. **Never enable the GPAMP**: it shares this pin |
| 41 | PB20 | GPIO out | `RLD_R_EN` | Pull-down R49 |
| 46 | PA26 | OPA0\_IN0+ (not selected) | `PD_BIAS` | Divider not fitted, so leave the pin analog and bias OPA0 from DAC8 |
| 47 | PA27 | OPA0\_IN0− | `PD_SUM` | Photodiode cathode. Analog |
| 48 | VCORE | regulator | — | 470 nF only |

- **Unused:** PA6, PA7, PA21, PA23, PA24, PA25 and PB24 have no connection. Set them as outputs driven low.
- **PA15:** the DAC12 output pin, also unconnected. Leave it in analog mode, since DAC12 reaches OPA1 internally.
- **Analog pins:** IOMUX function 0, with the digital input buffer off.

### Clocks

- **MCLK:** 80 MHz from SYSPLL (fed by SYSOSC) during acquisition. 32 MHz SYSOSC otherwise.
- **SYSOSC frequency correction:** internal resistor only. PA2 would be the external ROSC pin, but it is `FAST_RESTORE`.
- **LFCLK:** internal LFOSC. There is no LFXT, because PA3 and PA4 are GPIO.
  - LFOSC is only accurate to a few percent, so set the wall clock from the phone over BLE.
- **Common time base:** every acquisition timer runs from the same BUSCLK, so ECG and PPG samples share one timeline.

### Analog

| Block | Configuration |
| --- | --- |
| ADC0 (ECG) | Channel 12. Reference VDDA, 12-bit, sample time about 2 µs. Hardware averaging ×4. Triggered by a timer event at 4 kHz. DMA to a RAM ring buffer |
| ADC1 (PPG) | Sequence: channel 8 (`TIA_OUT`), channel 1 (`PPG_HI`). Channel 4 (`VMID_MON`) occasionally. Reference VDDA, averaging ×4, triggered by TIMA0 compare events. DMA |
| OPA0 (TIA) | General-purpose mode, no internal gain. Inverting input = IN0− (PA27). Non-inverting input = COMP0's DAC8. Output to PA22. Chopping on, high-bandwidth mode |
| COMP0 / DAC8 | Enable COMP0 only to power its 8-bit DAC. Reference VDDA, code 19 (≈ 0.245 V), the photodiode bias |
| OPA1 (second stage) | Inverting input = IN1− (PA17). Non-inverting input = DAC12. Output to PA16. Rail-to-rail input on |
| DAC12 | Reference VDDA. Sets the PPG offset (section 7) |

- **ADC reference:** use VDDA (`+3V3`). The `VREF+` pin has no capacitor, so the internal reference isn't an option on this board.
- **Why VDDA is fine:** `VMID` comes from the same rail family, so readings stay ratiometric. Measure `VMID_MON` to calibrate mid-scale.

### Timers, events and DMA

| Resource | Use |
| --- | --- |
| TIMA0 | PPG frame master. C3 drives `LED_BLANK` (PA12) with inverted polarity. Two pin-less compare channels publish "mid-dark" and "mid-LED-on" events that trigger ADC1 |
| TIMG (any) | 4 kHz event that triggers ADC0 for ECG |
| RTC | Wall clock, plus periodic wake from STANDBY |
| SysTick | 1 ms scheduler tick in RUN |
| DMA | ADC0 → RAM, ADC1 → RAM, UART0 TX, SPI1 TX (display frame), optionally UART0 RX |

- **Starting the timers:** start TIMA0 and the ECG timer from one event, or record both counters at start. A fixed, known offset between ECG and PPG samples is what makes pulse-arrival time meaningful.

### Serial peripherals

- **UART0:** to the radio. 8N1, no flow control. Baud rate, framing and wake handshake are in section 5. Use the FIFO, DMA and the RX timeout interrupt.
- **I2C0:** controller at 400 kHz. MCP4728 at 0x60, CW2015 at 0x62. MCP4728 `LDAC` is tied low, so DAC writes take effect immediately. Its `RDY` pin is not wired to the MCU; poll its status over I²C after an EEPROM write.
- **SPI1:** controller, mode 0, 8-bit, MSB first, 8 MHz. The display allows 10 MHz at most. CS and DC are GPIO.

### Interrupts and wake

- **Wake sources from STOP and STANDBY:**
  - buttons (PA31, PB15);
  - `RADIO_IRQ` (PB16);
  - `CHG_PG` (PA28, USB plugged or unplugged);
  - `CW_ALRT` (PA13);
  - the RTC.
- **Bounce and wake latency:** enable FastWake on these pins. A SHUTDOWN wake restarts the firmware, so read the SYSCTL reset cause to tell a button wake from power-on.
- **SHUTDOWN wake pins:** TI's datasheet tables disagree on which pins can wake from SHUTDOWN. Confirm PA31 in SysConfig before relying on SHUTDOWN; otherwise use STANDBY as the deepest state.

### Debug and bootloader

- **SWD:** XDS110 through Tag-Connect J1 (TC2030 footprint). Pin 1 = `+3V3` sense; the card supplies its own power.
- **ROM bootloader (BSL), manual entry:** fit JP1, or hold PA18 high through reset.
- **BSL over the radio:** the BSL's default UART pins on this family are PA10/PA11, which is the radio link. That lets the CH585 act as BSL host for over-the-air MSPM0 updates (section 5). Confirm the pins in TI's MSPM0 bootloader user guide before building on it.

## CH585 configuration

The CH585 runs only the BLE stack and the UART bridge, from WCH's TMOS-based BLE SDK. It has its own 32 MHz and 32.768 kHz crystals and a DC-DC inductor, and the MSPM0 switches its whole supply.

### Pin map (QFN-32)

| Pin | Port | Use | Net / part | Notes |
| --- | --- | --- | --- | --- |
| 1, 32 | PA10, PA11 | X32KI / X32KO | X3, 32.768 kHz, 12.5 pF (Epson FC-135R) | BLE sleep clock |
| 21, 22 | X32MO, X32MI | 32 MHz crystal | X2, 12 pF, ±10 ppm | RF reference |
| 3 | VSW | DC-DC switch | L3 (10 µH) |  |
| 2, 25 | VDCID, VDCIA | DC-DC output | 4.7 µF + 1 µF + 100 nF | Bypass R86 is not fitted, so the board is built for DC-DC mode |
| 4 | VIO33 / VDD33 | supply | `RADIO_VCC` | Switched by U17 |
| 23 | VINTA | internal regulator | C52 |  |
| 24 | ANT | RF | C79 10 pF, then pi network C47 / R56 / C40 to AE1 | C40 and C47 are tuning spots, not fitted |
| 9 | PB15 | TCK (2-wire debug) | `CH1_SWCLK` | Tag-Connect J3 |
| 10 | PB14 | TIO (2-wire debug) | `CH1_SWDIO` | Tag-Connect J3 |
| 11 | PB13 | TXD1\_ (UART1, remapped) | `CH1_TXD1` | To MSPM0 PA11 through 1 kΩ |
| 12 | PB12 | RXD1\_ (UART1, remapped) | `CH1_RXD1` | From MSPM0 PA10 through 1 kΩ. Also the wake input (section 5) |
| 18 | PB4 | GPIO out, push-pull | `RADIO_IRQ` | Active high, idle low |
| 20 | PB22 | GPIO in | `CH1_BOOT` | 10 kΩ pull-up to `RADIO_VCC`, TP4 to ground it |
| 5–8, 13–17, 19, 26–31 | NFC, USB and spare pins | unused | — | Input with pull-up, or output low |

### SDK settings (`CONFIG.h` and init code)

| Setting | SDK default | Use | Why |
| --- | --- | --- | --- |
| `DCDC_ENABLE` | FALSE | **TRUE** | L3 is fitted; DC-DC cuts radio current |
| `HAL_SLEEP` | FALSE | **TRUE** | Sleep between BLE events |
| `CLK_OSC32K` | 1 (internal) | **0** (external 32.768 kHz) | X3 is fitted; more accurate sleep timing |
| `BLE_BUFF_MAX_LEN` | 27 | **251** | Data-length extension for streaming (allowed range 27–516) |
| `BLE_TX_POWER` | 0 dBm | start at 0 dBm | Raise only if range with a hand on the card is short |
| `TEM_SAMPLE`, `BLE_CALIBRATION_ENABLE` | TRUE | keep | Temperature calibration of the RF |
| `BLE_MEMHEAP_SIZE` | 6 KB | raise as the GATT table grows |  |

- **UART1 remap:** set `RB_PIN_UART1` in `R16_PIN_ALTERNATE`, which moves RXD1/TXD1 from PA8/PA9 to PB12/PB13. Then configure PB13 as a push-pull output and PB12 as an input with pull-up.
- **32.768 kHz load capacitance:** `R8_XT32K_TUNE`, field `RB_XT32K_C_LOAD`. It resets to 1100b (about 24 pF), which is wrong for this 12.5 pF crystal.
  - Start at 0000b (12 pF).
  - Trim by measuring RTC drift against the MSPM0 over a few minutes.
- **32 MHz load capacitance:** `R8_XT32M_TUNE`, field `RB_XT32M_C_LOAD`. It resets to 10 pF.
  - Start at 0001b (12 pF).
  - Trim by measuring the carrier frequency offset with a test tone. Aim for under ±10 ppm.
- **Option bytes for development:** `CFG_DEBUG_EN` = 1, `CFG_RESET_EN` = 0, `CFG_ROM_READ` = 1 keeps 2-wire debug working. `CFG_RESET_EN` must stay 0 either way: it would turn PB23 into a reset pin, and PB23 is unconnected. `CFG_BOOT_EN` = 1 keeps the bootloader.

### Programming

- **2-wire debug:** WCH-LinkE through J3, a TC2030-IDC-NL pad. J3 carries `RADIO_VCC`, `CH1_SWDIO`, `CH1_SWCLK` and GND; you'll need a Tag-Connect-to-WCH-Link adapter wired to those four.
- **Power while programming:** run the radio from the card, with the MSPM0 holding `RADIO_EN` high, and set the WCH-LinkE not to supply 3.3 V. With U17 off, its output-discharge pull-down would load the probe's supply.
- **Bootloader:** on other CH58x parts, PB22 held low at power-up enters the bootloader. Confirm this for the CH585 with WCHISPTool before relying on TP4. The bootloader's serial mode almost certainly uses default (unremapped) UART pins, which aren't wired here, so plan on 2-wire debug plus BLE OTA rather than serial ISP.

### Firmware structure

- **TMOS tasks:**
  - BLE peripheral and GATT server (section 9);
  - UART bridge: ISR into a ring buffer, then a COBS frame parser;
  - link manager: `RADIO_IRQ`, wake handshake, HELLO frame;
  - OTA.
- **Downlink path:** notifications are queued from MSPM0 frames. If the BLE link stalls, keep about 1 s of data, drop the oldest frames and count the drops.
- **Uplink path:** phone writes become command frames to the MSPM0. The radio never acts on measurement settings itself.

## Inter-processor link

The MSPM0 is the master: it powers the radio, owns all settings, and sends data as framed packets over one UART. The CH585 raises `RADIO_IRQ` when it needs attention.

### Physical layer

- **UART:** 8N1, no flow control. Start at **460800 baud** and measure the actual divider error at both ends; it must be under 1 %.
  - The link needs only about 2 kB/s, so a slower rate is fine if one end can't hit 460800.
- **1 kΩ series resistors** (R57, R58) limit current into an unpowered pin, but don't eliminate it.
- **Pins while the radio is off:** drive MSPM0 `PA10` (TX) low and set `PA11` (RX) to input with pull-down. Otherwise UART idle-high back-feeds about 3 mA into the CH585 and partly powers it.

### Power-up and power-down

1. Set `PA10` to UART TX, `PA11` to RX, and `PB16` to input with pull-down and a rising-edge IRQ.
2. Drive `RADIO_EN` (PB18) high.
3. Wait for the radio's HELLO frame (firmware versions and BLE address). Time out after 500 ms, retry once, then report "radio fault".
4. **To power down:** send `RADIO_CTRL: shutdown`, wait for the ACK or 50 ms, then stop UART0. Return `PA10`/`PA11` to the off-state above and drive `RADIO_EN` low.

### Wake handshake

With `HAL_SLEEP` on, the CH585 sleeps between BLE events and can't receive while asleep. The MSPM0 can be in STANDBY with UART0 off.

- **Radio → MCU:**
  1. The radio drives `RADIO_IRQ` high while it has a frame or event pending.
  2. It waits `T_WAKE` before transmitting, giving the MSPM0 time to wake and enable UART0.
  3. It drops `RADIO_IRQ` once its queue is empty.
- **MCU → radio:**
  1. The MSPM0 sends one 0x00 byte, whose falling edge on PB12 is the CH585's wake source.
  2. It waits `T_WAKE`, then sends the frame. Leading zeros are harmless, because 0x00 is the frame delimiter.
- **`T_WAKE`:** start at 2 ms and shorten it at bring-up.
- **While streaming:** the radio may stay awake for the whole connection, which skips the handshake at a few mA extra. Decide at bring-up from measured currents.

### Framing

Each frame is COBS-encoded and ends with a 0x00 delimiter. Decoded, it reads:

| Field | Size | Notes |
| --- | --- | --- |
| type | 1 B | See message table |
| seq | 1 B | Per-direction counter; the receiver counts gaps |
| len | 2 B, little-endian | Payload length, 0–512 |
| payload | len |  |
| crc | 2 B, little-endian | CRC-16/CCITT-FALSE over type, seq, len and payload |

Frames with a bad CRC are dropped and counted. Commands are acknowledged; data blocks are not.

### Messages

| Type | Name | Direction | Payload |
| --- | --- | --- | --- |
| 0x01 | HELLO | radio → MCU | Firmware version, BLE address, reset reason |
| 0x02 | STATUS | MCU → radio | State, battery %, charging, USB present, lead-off, settings |
| 0x10 | ECG\_BLOCK | MCU → radio | Start sample index (u32), gain flag, 10 × int16 at 500 Hz |
| 0x11 | PPG\_BLOCK | MCU → radio | Start frame index (u32), LED current code, 2 × (on − dark, fine) int16 |
| 0x12 | EVENT | MCU → radio | R-peak index, heart rate, pulse arrival time, button, lead-off |
| 0x20 | CMD | radio → MCU | Start or stop ECG/PPG, gain, notch 50/60 Hz, RLD mode, LED limit, set time |
| 0x21 | ACK / NACK | both | Echoed seq, result code |
| 0x30 | LOG | both | Debug text, only in development builds |
| 0x40 | RADIO\_CTRL | MCU → radio | Advertise on/off, disconnect, device name, shutdown |
| 0x41 | CONN\_STATE | radio → MCU | Connected/disconnected, MTU, connection interval |
| 0x50 | BSL\_BRIDGE | both | MSPM0 firmware update traffic (below) |

- **Timing:** the MSPM0 sends one ECG block and two PPG frames every 20 ms, to match the BLE connection interval (section 9).
- **Timestamps:** every block carries its sample index from the master timer, so the phone can rebuild exact timing even if frames are dropped.

### Firmware updates

- **CH585:** WCH's BLE OTA (IAP) scheme, with the 2-wire debug link (J3) as recovery.
- **MSPM0 over the air:**
  1. The phone sends the MSPM0 image to the radio in BSL\_BRIDGE frames.
  2. The radio sends `CMD: enter bootloader`.
  3. The MSPM0 performs DriverLib's bootloader-entry reset.
  4. The radio then speaks TI's BSL UART protocol on the same pins and verifies the CRC before resetting the MSPM0.
  - This depends on the BSL using PA10/PA11 by default (section 3). Confirm it, and the BSL's starting baud rate, in TI's MSPM0 bootloader guide.
- **Recovery:** SWD (J1) and JP1 always work.

## ECG acquisition

ECG is sampled at 4 kHz, decimated to a 500 Hz stream with a 0.5–150 Hz band, and only runs on battery power.

### Start sequence

1. If `CHG_PG` is low, show "Unplug USB to measure ECG" and stop. The hardware would block it anyway.
2. Drive `ANA_EN` and `BUCK_PS_SYNC` high.
3. Wait at least 300 ms for `VMID` (50 ms time constant). Then read `VMID_MON` and check it is within ±10 mV of mid-scale.
4. Set `GAIN_HI` (default low, 558×) and `RLD_L_EN` = `RLD_R_EN` = high, both thumbs driven. Keep `FAST_RESTORE` low.
5. Drive `MEASURE_REQ` high. `BODY_EN` (TP15) goes high and the electrodes connect.
6. Start the ECG timer, ADC0 and DMA.
7. **Contact detection:** while the thumbs are off, the output sits near a rail or wanders. When it comes off the rail, pulse `FAST_RESTORE` high for 20–50 ms. If the baseline is still more than 25 % of full scale from mid after 500 ms, pulse it once more.
8. **Settling:** the first stage's AC coupling settles over about 2–3 s (0.5 s time constant). Flag samples as settling until then.

### Stop sequence

1. Drive `MEASURE_REQ` low.
2. Drive `FAST_RESTORE`, `GAIN_HI`, `RLD_L_EN` and `RLD_R_EN` low.
3. Stop the timer and ADC0.
4. Drive `BUCK_PS_SYNC` low, and `ANA_EN` low unless PPG is running.

No control pin may stay high into the unpowered analog rail.

### Sampling and scaling

| Item | Value |
| --- | --- |
| ADC0 | Channel 12, triggered at 4 kHz, hardware average ×4 per trigger |
| Decimation | Software FIR, 8:1 to 500 Hz. Passband to 150 Hz, at least 40 dB down at 250 Hz |
| Resolution | 0.806 mV per code at the ADC. Input-referred: 1.44 µV (558×) or 0.73 µV (1098×) |
| Input range | ±2.96 mV (558×) or ±1.50 mV (1098×) around `VMID` |
| Conversion | µV = (code − mid code) × 805.7 ÷ gain. Take mid code from `VMID_MON`, not 2048 |

The hardware's 339 Hz anti-alias filter is first order. It works because the ADC samples at 4 kHz and the digital filter does the steep cut before decimation.

### Processing

- **Stream to the phone:** the 500 Hz, 0.5–150 Hz data, unfiltered beyond decimation.
- **Display path:**
  - a 0.5 Hz high-pass for residual drift;
  - a 50 or 60 Hz notch, from a user setting or picked by comparing the two bands;
  - a 40 Hz low-pass.
- **R-peaks:** Pan–Tompkins on the 500 Hz stream. Heart rate = median of the last 5 R–R intervals.
- **Lead-off:** the output within 5 % of either rail for over 100 ms means no contact. A large 50/60 Hz residual means poor contact.
- **Gain:** if the QRS is under about 25 % of the range for 5 s at 558×, switch to 1098×. Change it only between records, and pulse `FAST_RESTORE` after, because the switch injects charge.

### Right-leg drive

| `RLD_L_EN` | `RLD_R_EN` | Mode |
| --- | --- | --- |
| 1 | 1 | Both thumbs. Default: most contact area, best mains rejection |
| 1 | 0 | Left thumb only |
| 0 | 1 | Right thumb only |
| 0 | 0 | No right-leg drive; two-electrode mode for comparison |

- **Start:** with both islands on.
- **Bad side:** if one side's signal collapses or follows the right-leg drive (for example a sweat film bridging the ring and island), try the other two on-states. Keep whichever gives the least mains noise and steadiest baseline.
- **Switching cost:** each switch injects a few pC. Switch between records, or follow with a `FAST_RESTORE` pulse.

### Interlock during a record

If `CHG_PG` falls mid-record (USB plugged in), the hardware disconnects the electrodes at once. The PA28 interrupt must end the record, mark it "stopped: USB connected", and run the stop sequence.

## PPG acquisition

PPG runs as 100 Hz frames: one dark sample and one LED-on sample per frame, both timed by TIMA0, with the LEDs on for 2 % of the time. Unlike ECG, PPG has no galvanic path to the body, so it may run while USB is connected.

### Start sequence

1. Drive `ANA_EN` high: the LED current sinks run from `+3V3_SW`. Drive `BUCK_PS_SYNC` high. Keep `LED_BLANK` high.
2. Enable COMP0's DAC8 (code 19, ≈ 0.245 V photodiode bias), OPA0, DAC12 and OPA1.
3. Drive `VLED_EN` high. Wait at least 5 ms, then check that the boost has started (`VLED` test point TP8 at bring-up).
4. Power up MCP4728 channel A and write the starting current, about 5 mA per LED.
5. Start TIMA0. From here it drives `LED_BLANK` and triggers ADC1 every frame.

**Stop** in reverse: stop TIMA0 with `LED_BLANK` high, power down DAC channel A, drive `VLED_EN` low, then disable the OPAs and DACs.

### Frame timing (start values)

| Time in frame | Event |
| --- | --- |
| 0 | Frame start; LEDs off since the last frame |
| 9.70 ms | ADC1 "dark" trigger: `TIA_OUT`, then `PPG_HI` |
| 9.80 ms | `LED_BLANK` low: LEDs on |
| 9.95 ms | ADC1 "on" trigger: `TIA_OUT`, then `PPG_HI` |
| 10.00 ms | `LED_BLANK` high: LEDs off, next frame starts |

- **Why 150 µs before the on-sample:** the TIA (1 MΩ × 10 pF) and the second stage (499 kΩ × 22 pF) each have about a 10 µs time constant, and 12-bit settling needs about 8 of them. The rest covers the current sinks' rise. Shorten it once the edges have been measured.
- **Ambient subtraction:** the dark and on samples are 250 µs apart, so subtracting them also removes 100/120 Hz flicker from mains lighting.

### Signal path

| Signal | Formula | Resolution |
| --- | --- | --- |
| Photocurrent | I = (`TIA_OUT` − 0.245 V) ÷ 1 MΩ | 0.81 nA per code |
| Second stage | `PPG_HI` = 11 × V\_DAC12 − 10 × `TIA_OUT` (inverted) | 81 pA per code |
| Coarse PPG | `TIA_OUT`(on) − `TIA_OUT`(dark) | DC level and perfusion index |
| Fine PPG | −`PPG_HI`(on), inverted back | Pulsatile waveform |

- **`PPG_HI` during the dark phase:** it railing is expected. A later version can update DAC12 by DMA on each phase, so both samples stay in range for full-resolution subtraction.
- **Both LEDs** share `GREEN_SET`, so they always carry the same current.

### Control loops

- **DC cancellation, about once per second:** set V\_DAC12 = (1.65 V + 10 × `TIA_OUT`(on)) ÷ 11, which keeps `PPG_HI`(on) near mid-scale.
- **LED current (AGC):**
  - If `TIA_OUT`(on) goes above 2.8 V, step the current down by about 20 %.
  - If it falls below 0.6 V, step it up by about 25 %.
  - Target 1.5–2.0 V.
  - Change the current only between frames, and flag that frame so processing can skip the step.
- **Current limit:** cap the DAC code so neither LED exceeds 20 mA.
  - On the DAC's VDD reference, full scale is 24.5 mA.
  - On its internal 2.048 V reference, full scale is 15.2 mA, and the light no longer follows `+3V3` ripple. Prefer the internal reference unless 15 mA isn't enough light.

### MCP4728 setup

- **Once, at first boot:** write EEPROM defaults with every channel powered down (PD = 01: 1 kΩ to ground, which also holds `GREEN_SET` low) and the chosen reference.
- **In use:** Fast Write or Multi-Write commands only; don't write the EEPROM routinely. Keep channels B–D powered down.

### Outputs

- **Stream:** PPG waveform at 100 Hz.
- **Derived values:** pulse rate, and perfusion index = AC ÷ DC.
- **Pulse arrival time:** the delay from each ECG R-peak to the PPG pulse foot, found with the intersecting-tangent method on the rising edge. It tracks blood pressure and arterial stiffness as a relative indicator only, not a blood-pressure reading.

**Display interference:** the OLED module sits right over the TIA, with its charge pump running. Compare PPG noise with the display on and off at bring-up. If it matters, blank the display (`AEh`) during PPG captures.

## Power management

The card sleeps in STANDBY with only `+3V3` up, and turns rails on per activity. The TPS63001's quiescent current, about 40 µA, sets the floor.

### Operating modes

| Mode | MSPM0 | Rails on | Radio | Measure at bring-up |
| --- | --- | --- | --- | --- |
| Off / standby | STANDBY, RTC running | `+3V3` | off | Target < 80 µA from the cell: TPS63001 \~40 µA, CW2015 \~15 µA, charger battery leakage ≤ 6.5 µA. 500 mAh then lasts about 8 months |
| UI idle | RUN or SLEEP | `+3V3`, `OLED_VCC` | off, or advertising |  |
| ECG | RUN, 80 MHz | `+3V3`, `+3V3A`, `OLED_VCC` | optional, streaming |  |
| PPG | RUN, 80 MHz | `+3V3`, `+3V3A`, `VLED`, `OLED_VCC` | optional, streaming |  |
| Charging | any | as for the activity | any | ECG blocked |

- **Suggested timeouts:** display off after 60 s without input, STANDBY after 5 min.
- **Long press:** BUTTON1 held for 2 s goes straight to STANDBY with the radio off.

### Rail sequencing

The rule: no output drives into a rail that is off.

- **Before `ANA_EN` goes low:**
  - `MEASURE_REQ`, `FAST_RESTORE`, `GAIN_HI`, `RLD_L_EN` and `RLD_R_EN` low;
  - `LED_BLANK` high;
  - DAC channel A powered down;
  - TIMA0 stopped.
- **Before `VLED_EN` changes:** `LED_BLANK` high.
- **Display power-up:**
  1. Drive `OLED_RESET` low, then `OLED_EN` high.
  2. Wait at least 5 ms, then release reset.
  3. Send the module's init sequence (datasheet section 4.2), then `8Dh, 14h` (charge pump on) and `AFh` (display on).
  4. Allow 100 ms before relying on the image.
- **Display power-down:**
  1. Send `AEh`, then `8Dh, 10h`, and wait 100 ms.
  2. Drive SCK, MOSI, DC, CS and RES low.
  3. Drive `OLED_EN` low.
  - SCK and MOSI have only 33 Ω in series, so a high pin would push tens of mA into the unpowered module.
- **Radio:** see section 5.

### Charger and USB

- **`CHG_PG` interrupt, both edges:**
  - Falling (plugged in): end any ECG record, show the charging state, report it in STATUS.
  - Rising (unplugged): clear it.
- **`CHG_STAT1`:** decode with `CHG_PG` as in section 2. A 2 Hz toggle for more than 2 s means a safety-timer fault; show it, and clear it by unplugging.
- **Firmware's role:** the charger needs no configuration. Firmware only reports.

### Fuel gauge (CW2015, 0x62)

1. **Version:** read VERSION (0x00), expected 0x73.
2. **Wake:** write 0x00 to MODE (0x0A). It powers up asleep (0xC0).
3. **Battery profile:** if CONFIG (0x08) bit 1 (UFG) is clear, or the stored profile differs, write the 64-byte profile to 0x10–0x4F, set UFG, and restart the gauge.
   - Get the profile for the chosen cell from CellWise or the cell vendor. Without it, state-of-charge accuracy suffers.
   - The Linux kernel's `cw2015_battery` driver is a good reference for the exact sequence.
4. **Alert threshold:** set ATHD (CONFIG bits 7:3), for example to 5 %. The alert arrives on `CW_ALRT` (PA13). Clear it by writing 0 to RRT\_ALERT bit 15.
5. **Polling:** every 10–60 s, read VCELL (0x02–0x03, 14-bit, 305 µV per code) and SOC (0x04 = whole %, 0x05 = 1/256 %).

### Low battery

- **Under 5 %:** stop measurements and show a warning.
- **Under 3 %, or VCELL under 3.3 V:** go to STANDBY with the radio off.
- **Cell protection:** the board's battery protection IC (U16) is not fitted, and R60 bypasses it. The cell must have its own protection board. Firmware's cut-off is only a second line of defence.

### Buttons

- **Debounce:** 20 ms in software, on top of the 10 nF capacitors.
- **Proposed mapping:**

  | Button | Short press | Long press (2 s) |
  | --- | --- | --- |
  | BUTTON1 (PA31) | select / wake | power off |
  | BUTTON2 (PB15) | next | back |
- **Stuck-button check:** if a button reads pressed continuously from boot, report "button fault". See section 12 on the switch pad pairing.

## Application behaviour

The MSPM0 runs one state machine: standby, menu, then ECG, PPG or both. BLE streaming is an option on top of any measurement, not a separate mode.

&#91;embedded content: application states · 5 states, 2 global rules\]

The phone can start and stop measurements too, through Control commands. The MSPM0 applies the same rules whatever started them.

### Screens

1. **Home:** battery %, charging, BLE status, time.
2. **ECG:** live trace, heart rate, contact status. Shows "Unplug USB" while charging.
3. **PPG:** live trace and pulse rate.
4. **ECG + PPG:** both traces, heart rate, pulse arrival time.
5. **Settings:** gain (auto / 558× / 1098×), mains notch (50 / 60 Hz), right-leg drive mode, LED current limit, BLE on/off.
6. **About:** firmware versions of both chips, battery voltage.

The display is 128 × 64 with a 1 KB frame buffer. Send it by SPI DMA, and redraw at no more than 25 Hz during measurements.

### BLE behaviour

- **Advertising:** connectable, named "ECG Card XXXX" (last 4 hex digits of the BLE address). Every 100 ms for 30 s, then every 1 s. Stop after 2 min unconnected, and power the radio off until the user wakes the card or turns BLE on.
- **Connection:** request a 20 ms interval, latency 0 while streaming, a 4 s supervision timeout, data-length extension (251) and an ATT MTU of 247.
  - 20 ms is exactly 10 ECG samples at 500 Hz and 2 PPG frames at 100 Hz. Any residual radio interference then lands at a fixed phase in the data, where it is easy to notch.
- **Throughput:** ECG 1000 B/s plus PPG 400 B/s, about 1.6 kB/s with headers. That is one notification of about 32 B for ECG and one of about 12 B for PPG per interval.

### GATT table

| Service | UUID | Characteristics |
| --- | --- | --- |
| Device Information | 0x180A | Manufacturer, model, both firmware revisions, serial number |
| Battery | 0x180F | Battery Level 0x2A19 (read, notify) |
| Heart Rate | 0x180D | Heart Rate Measurement 0x2A37 (notify), from ECG R-peaks, or PPG when ECG is off |
| ECG Card (custom) | 128-bit, to assign | ECG stream (notify), PPG stream (notify), Events (notify), Status (read, notify), Control (write with response) |
| OTA | WCH's OTA service | CH585 image; MSPM0 image through the BSL bridge |

- **Payloads:** stream payloads reuse the UART payload formats (section 5), so the radio forwards them without parsing.
- **Pairing:** Just Works bonding is enough. The card is not a medical device and stores no personal data.

### Storage

- **Settings:** the last MSPM0 flash sector, with a CRC. Write only when a setting changes.
- **Data:** no record storage in this version. Keep a few seconds of ECG and PPG in RAM for the display; there are 32 KB of SRAM in total.

## Bring-up plan

Bring the card up one block at a time, in this order, each with a pass check before the next.

**Power during bring-up:** use a current-limited bench supply in place of the cell (3.7 V, 100 mA limit) until step 3. Whenever the electrodes are involved, run the debugger's laptop on battery: the interlock only sees the card's own USB port, not a ground loop through the debugger.

| Step | Do | Pass when |
| --- | --- | --- |
| 0. Bare board | Continuity checks before power | No rail shorted to GND. Left and right rings show equal capacitance to GND |
| 1. Power path | Supply into H1 (R93 is the battery link) | `VSYS` ≈ supply, `+3V3` = 3.30 V. All switched rails off |
| 2. MSPM0 | Flash a blinky over J1. Read `CHG_PG`, `CHG_STAT1` and both buttons | Pins read their pulled defaults; buttons toggle; no stuck press |
| 3. Charger | Plug in USB-C with a real cell fitted | `CHG_PG` low, `CHG_STAT1` low, about 270 mA into the cell, `VSYS` ≈ 4.4 V. With `MEASURE_REQ` high, TP15 (`BODY_EN`) stays low |
| 4. I²C | Scan the bus | ACKs at 0x60 (MCP4728) and 0x62 (CW2015). CW2015 VERSION = 0x73 |
| 5. Display | Power sequence, init, test pattern | Image stable; module straps confirmed as 4-wire SPI |
| 6. Analog rail | `ANA_EN` high | `+3V3A` ≈ 3.28 V, `VMID_MON` ≈ 1.65 V, `ECG_ADC` near mid-scale |
| 7. ECG, simulator | 1 mV, 10 Hz differential sine into the rings through a resistive divider, right-leg drive to the divider midpoint | Gain 558× and 1098× within 3 %; −3 dB near 0.5 Hz and 150 Hz. Record the shorted-input noise as the baseline |
| 8. ECG, thumbs | Battery only | QRS visible; no oscillation on the right-leg drive; compare the four RLD modes; `FAST_RESTORE` recovers the baseline |
| 9. LED drive | `VLED_EN` high; set a low current | `VLED` (TP8) ≈ 4.99 V. V(`ISNS1`) and V(`ISNS2`) (TP12, TP13) = I × 8.2 Ω. Clean edges on `LED_BLANK` |
| 10. PPG | Thumb on the window | Pulsatile signal after ambient subtraction. With the window covered in black tape, the on − dark residue is small: that is the optical leak test |
| 11. Radio | Hold `RADIO_EN` high from a debug build; program the CH585 over J3 | HELLO frame at the MSPM0; UART at 460800; advertising visible; both crystals trimmed |
| 12. System | Stream ECG and PPG to a phone for 10 min | No dropped frames; no new tones in the ECG spectrum; current measured in every mode |

Keep the bring-up firmware as a permanent build target. Each step above should be a command it accepts, so it doubles as the production test. The MSPM0's only UART goes to the radio, so drive commands through the debugger (a command word in RAM) until step 11, then over BLE.

## Testing and validation

Test the DSP on a PC first, then the hardware against simulators, then the whole card on people. Set the numeric pass limits from the first good boards, and regress every later build against them.

| Area | Method | Checks |
| --- | --- | --- |
| DSP, on a PC | Unit tests on recorded and synthetic data, e.g. PhysioNet ECG records for QRS detection | Decimator response; notch depth at 50 and 60 Hz; R-peak sensitivity and false positives; PPG foot detection; pulse arrival time on synthetic pairs |
| Link | Unit tests plus fuzzing on both ends | COBS and CRC round-trips; sequence-gap counting; recovery from bytes lost mid-frame; behaviour when the radio is powered off mid-stream |
| ECG front end | Signal generator through a resistive divider into the rings | Gain at both settings; frequency response; shorted-input noise; recovery time after `FAST_RESTORE` |
| Mains rejection | Inject 50/60 Hz common-mode through matched electrode impedances, then with one side unbalanced | Rejection in each of the four right-leg-drive modes |
| Lead-off and restore | Make and break thumb contact repeatedly | Lead-off flagged within 200 ms; baseline back within about 3 s of contact |
| PPG | Optical target (white silicone or a paper stack), then fingers; a flickering mains lamp | LED current against DAC code; ambient rejection at 100/120 Hz; covered-window leak test; AGC stability |
| Power | Source meter or power profiler in every mode | Standby current; per-mode current; projected battery life; one full charge cycle with timer, termination, `CHG_PG` / `CHG_STAT1` and chip temperature |
| BLE | Phone and desktop client | 1 h streaming with no gaps; range with a hand on the card; reconnection; OTA for both chips, including a failed-transfer recovery |
| Safety | Required on every build | USB plugged + `MEASURE_REQ` high leaves TP15 low; plugging USB mid-record ends it; after reset, watchdog or brown-out every enable reads low |
| Robustness | Fault injection | Watchdog recovery; I²C bus hang recovery (9 clocks, then re-init); corrupted settings fall back to defaults; button chatter; radio missing or not answering |

Automate as much as possible as hardware-in-the-loop on one reference card: a signal generator, a USB power switch and the debug probes. Run it on every firmware change.

## Open questions and risks

None of these blocks starting firmware, but each needs an answer from the first boards or a datasheet check.

| # | Question | Why it matters | How to settle it |
| --- | --- | --- | --- |
| 1 | Are the TS-1075S switch pads 3 and 4 joined internally? | If not, the buttons read permanently pressed | Meter a loose switch; step 2 of bring-up |
| 2 | Can PA31 wake the MSPM0 from SHUTDOWN? | Decides whether SHUTDOWN or STANDBY is the off state | SysConfig pin attributes; test on the bench |
| 3 | Can OPA0's non-inverting input take COMP0's DAC8, and is OPA0 quiet enough as the TIA? | The photodiode bias depends on it. The fallbacks are fitting the R74 / R75 / C68 divider on PA26, or an external TIA | SysConfig, then PPG noise at bring-up |
| 4 | Do the MSPM0 ROM bootloader's default UART pins and baud rate match PA10/PA11? | Decides whether MSPM0 updates can go over the air | TI's MSPM0 bootloader guide, then a bench test |
| 5 | Is PB22 the CH585's bootloader-entry pin, and which UART does its serial mode use? | Decides the radio's recovery path besides 2-wire debug | WCHISPTool and the CH585 documentation |
| 6 | Which battery profile goes into the CW2015? | State-of-charge accuracy | CellWise or the cell vendor, for the chosen 500 mAh cell |
| 7 | Crystal load settings on the CH585 | RF frequency offset and RTC accuracy | Trim both at bring-up, then fix the values in firmware |
| 8 | UART baud error at 460800 on both ends | Link reliability | Compute both dividers, then a frame-error soak test |
| 9 | Does the D8 ESD diode (a TECH PUBLIC part, not TI's) add drift? | Baseline wander as the thumbs warm the pads | Compare left and right baseline drift; swap D8 if they differ |
| 10 | Does the OLED, mounted over the TIA, add noise to PPG? | PPG signal quality | Compare noise with the display on and off |
| 11 | Is radio sleep with the wake handshake worth it while streaming? | A few mA of battery current against added complexity | Measure both ways in step 12 |

### Sources

- [TI MSPM0G3507 datasheet](https://www.ti.com/lit/ds/symlink/mspm0g3507.pdf) and [technical reference manual (SLAU846)](https://www.ti.com/lit/ug/slau846/slau846.pdf)
- [TI MSPM0 bootloader application note (SLAAE88)](https://ti.com/lit/pdf/slaae88)
- [TI E2E: PA14 as ADC0 channel 12](https://e2e.ti.com/support/microcontrollers/arm-based-microcontrollers-group/arm-based-microcontrollers/f/arm-based-microcontrollers-forum/1263442/mspm0g3507-adc0-a0_12---pa14-not-able-to-assign-to-pin-17-in-28-pin-package-via-syscfg)
- [TI bq2407x datasheet](https://www.ti.com/lit/ds/symlink/bq24075.pdf)
- [TI TPS61040](https://www.ti.com/lit/ds/symlink/tps61040.pdf), [TPS63001](https://www.ti.com/lit/ds/symlink/tps63001.pdf), [TPS22918](https://www.ti.com/lit/ds/symlink/tps22918.pdf), [TMUX1101](https://www.ti.com/lit/ds/symlink/tmux1101.pdf), [TLV9002](https://www.ti.com/lit/ds/symlink/tlv9001.pdf)
- [Microchip MCP4728 datasheet](https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/22187E.pdf)
- [CW2015 datasheet](https://github.com/qingehao/CW2015/blob/master/doc/CW2015_datasheet.pdf) and [Linux cw2015\_battery driver](https://github.com/torvalds/linux/blob/master/drivers/power/supply/cw2015_battery.c)
- [WCH CH585 repository: datasheet, BLE SDK and examples](https://github.com/openwch/ch585)
- [32 MHz crystal TAXM32M4RFBCCT2T (LCSC C141794)](https://www.lcsc.com/product-detail/C141794.html)
