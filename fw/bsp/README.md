# fw/bsp — `hal.h` on the MSPM0G3507

TI DriverLib (prebuilt `driverlib.a`, SDK 2.11.00.07), own startup and linker
script. Builds only when cross-compiling.

| File | Contents |
|---|---|
| `hal_core.c` | init, pins, GPIO ISR (CHG_PG safety path), clocks/SYSPLL, SysTick, RTC, WWDT, SLEEP/STANDBY |
| `hal_acq.c` | ECG/PPG time base, ADC0+DMA ring, ADC1 frames, PPG analog, one-shot ADC (design notes at the top) |
| `bsp_ring.h` | ECG ring index arithmetic as pure functions (no TI headers); host-tested by `fw/bsp/tests/test_bsp_ring.c` (`ctest -R bsp_ring`) |
| `hal_bus.c` | I2C0 (polled, 10 ms timeout, recovery), SPI1 OLED (blocking + DMA) |
| `hal_nv.c` | settings blob in the last flash sector |
| `rtt.c` | SEGGER-RTT-compatible control block (own implementation) |
| `startup_mspm0g3507.c`, `mspm0g3507.ld` | vectors, reset handler; FLASH = 127 KB, last 1 KB sector reserved (`NVSTORE`, 0x1FC00) |
| `smoke_main.c` | `ecg_bsp_smoke.elf`; debugger-driven command word `g_smoke_cmd` |
| `main.c` | `ecg_fw.elf` (only if `ecg_app` exists): `hal_init()` then `app_main()` |

`ecg_bsp` is an OBJECT library so its ISRs and `hal_*` symbols are always on
the link line (no archive-order or weak-vector surprises); `--gc-sections`
drops the unused rest.

## Resource allocation

| Resource | Use |
|---|---|
| MCLK | 32 MHz SYSOSC (FCL, internal resistor); 80 MHz = SYSPLL(SYSOSC) CLK2X, FCC lock check (SYSPLL_ERR_01). ULPCLK 32 / 40 MHz. Flash WS restored/2. |
| LFCLK | LFOSC (no LFXT, PA3/PA4 are GPIO; no ROSC, PA2 is GPIO) |
| TIMA0 (PD1) | PPG frame master, 4 MHz tick, LOAD 39999. CC0 = 1198 dark ADC trigger, CC1 = 198 on ADC trigger, CC3 = 798 → PA12 LED_BLANK (high on load, low at CC3). Cross-trigger master (TRIG0). |
| TIMG6 (PD1) | ECG, 4 MHz tick, LOAD 999, CC0 = 998 → ADC0 trigger. Started by TRIG0. |
| Event ch 1 | TIMG6 FPUB0 (CCD0) → ADC0 FSUB0 |
| Event ch 2 | TIMA0 FPUB0 (CCD0 \| CCD1) → ADC1 FSUB0 |
| ADC0 | ECG ch12 (PA14), VDDA, 12-bit, avg ×4, 2 µs (SCOMP0 = 64 @ SYSOSC 32 MHz), repeat-single, TRIGGER_NEXT |
| ADC1 | PPG sequence ch8 TIA_OUT, ch1 PPG_HI, avg ×4, 2 µs; one-shots (ch4 VMID etc.) |
| DMA ch0 (FULL) | ADC0 MEMRES0 → `s_ecg_ring[512]`, repeat-single (circular) |
| DMA ch3 (BASIC) | buffer → SPI1 TXDATA (OLED frame) |
| DMA ch1, ch2 (FULL), ch4–6 | free (UART0 RX/TX later) |
| COMP0 | DAC8 only (VDDA, code 19) → OPA0 + |
| OPA0 / OPA1 / DAC0 | TIA (IN0− PA27, out PA22, chop per `BSP_OPA0_CHOP`, GBW high) / 2nd stage (IN1− PA17, + = DAC12, out PA16, RRI) / offset DAC (VDDA) |
| GPAMP | forced off at init, never enabled |
| I2C0 | PA0/PA1, 400 kHz from ULPCLK (TPR 7 @ 32 MHz, 9 @ 40 MHz) |
| SPI1 | PB8 PICO, PB9 SCK, Motorola 3-wire mode 0, 8 MHz (SCR 1 @ 32, 4 @ 80 MHz) |
| RTC | LFOSC, prescaler-1 periodic 1 Hz interrupt → seconds counter, timed STANDBY wake |
| WWDT0 | LFCLK/2, 2^15 → 2.0 s, no closed window, runs in sleep, stops on debug halt; started first thing in `hal_init()` |
| SysTick | 1 ms (reloaded on clock switch) |

Interrupt priorities (0 = highest): GROUP1/GPIO 0 (CHG_PG drives MEASURE_REQ low in the ISR),
ADC0 0 (DMAEN re-arm, 250 µs deadline every 128 ms), ADC1 1 (PPG pairs, re-arm
before the on-trigger 250 µs after dark), SysTick 2, RTC 2. No other IRQs are enabled.

Low-power: `hal_idle()` = SLEEP0 (WFI). `hal_standby()` = **STANDBY0**: PD0
(GPIO, RTC, WWDT, I2C0) keeps ULPCLK/LFCLK, so the RTC interrupt and GPIO
edges (FastWake on BUTTON1/2, CHG_PG, CW_ALRT, RADIO_IRQ) wake it; STANDBY1
would stop ULPCLK to most PD0 modules, and TI's own RTC-in-standby example
uses STANDBY0. The CPU wakes for 1 Hz RTC ticks (kicks the watchdog, goes
back) and returns only on a real wake source or the timed deadline
(granularity 1 s). CHG_STAT1 is masked in STANDBY; a level change is reported on exit.

## RAM budget (BSP)

| Buffer | Bytes |
|---|---|
| ECG ring (512 × u16, 128 ms; ≥ 124 ms usable) | 1024 |
| PPG frame ring (32 × 8 B, 320 ms) + frame indices (32 × 4 B) | 384 |
| RTT up0 1 KB + up1 2 KB + down0 16 B + control block | 3184 |
| `.ramfunc` (DriverLib flash command loop) | 56 |
| state variables (incl. `bsp_ring_t`) | ~200 |
| main stack (top of SRAM, linker `__stack_size`) | 2048 |
| **total** | **≈ 6.9 KB** of 32 KB |

## Not verifiable without hardware (highest risk first)

1. **Timer start by cross trigger.** TIMA0.CTTRIG → TRIG0 → TIMA0 and TIMG6
   load (CCCTL0.LCOND = trigger, IFCTL0.ISEL = TRIG) and enable. Mirrors TI's
   `timx_timer_mode_pwm_x_trig_stop_restore` (TIMA1/TIMG7, TRIG1) but with
   TIMA0/TIMG6 and TRIG0 from SysConfig's index table. Scope LED_BLANK and an
   ADC trigger; check ECG-only start too.
2. **ADC0 repeat-single + TRIGGER_NEXT + HW avg ×4**: one 4-conversion burst
   per trigger. Expect exactly 4000 samples/s (if 1000/s, averaging needs a
   trigger per conversion).
3. **ECG DMA ring**: ADC clears DMAEN at each DMA lap in repeat mode and the
   ISR re-arms it; DMA reads of MEMRES0 clear MEMRESIFG0 (otherwise every lap
   / every sample counts as "lost" — visible as a steady `hal_ecg_overruns()`).
   The write position is taken from DMASZ (the TRM documents it as
   decremented per transfer) and cross-checked against DMADA, which the TRM
   only describes as copied to a temporary register in the repeated modes
   (if DMASZ turns out stuck while DMADA moves, DMADA is used automatically):
   the smoke image logs `dma_pos_mismatch` (non-zero = one of them is not live) and
   `resync` (`hal_acq_resyncs()`, must stay 0). More than two conversions lost
   at one lap end (ISR > 500 µs late) cannot be counted (MEMRES0 + one OVIFG
   flag) and would slip the timeline.
4. **PA22 = ADC1 channel 8** (from the brief; the SDK only confirms PA22 =
   ADC0 ch7, PA16 = ADC1 ch1, PA14 = ADC0 ch12 via the E2E note). Wrong
   channel = garbage TIA readings: change `TIA_ADC_CHAN`.
5. **ADC1 re-arm** of a non-repeating sequence by toggling ENC in the ISR.
6. **OPA0 PSEL = DAC8 with NSEL = IN0− (PA27)**: each mux leg appears in a TI
   LP-G3507 example, the combination does not; DAC8 kept alive with all COMP0
   inputs disconnected; TIA noise with chopping. Chopping is the CMake cache
   option `BSP_OPA0_CHOP` (1 = standard, the default per the brief; 0 = off):
   the TRM says standard chopping needs an external RC filter the board does
   not have, so compare both. ADC-assisted chopping is not offered (the TRM
   does not tie ADC1 ch8 / PA22 to OPA0's chop control).
7. **LED_BLANK**: CCPIV = high while TIMA0 is disabled (no low glitch when PA12
   is muxed to TIMA0_CCP3), LACT/CDACT polarity, 200 µs on-window at 9.8 ms.
8. **80 MHz**: PLL lock + FCC check, ULPCLK/2, I2C TPR 9, SPI SCR 4, flash WS 2.
9. **STANDBY0**: wake by RTC/GPIO FastWake, SPI1 re-init after wake, WWDT
   margin (1 s tick vs 2 s on the same LFOSC), standby current.
10. **I2C polling**: TX_DONE/RX_DONE raw flags with repeated START; auto-STOP
    after NACK (else re-init after 1 ms); zero-length probe (`wn = rn = 0`);
    9-clock recovery.
11. **Flash NV**: erase/program from RAM with IRQs masked (ECG samples may be
    lost and counted during a ~ms erase); no ECC fault reading erased flash.
12. **Reset cause**: cold power-up may read PORHWFAIL (mapped POWER_ON) or
    BORSUPPLY (mapped BROWNOUT).
13. SPI1 TX-only: RX FIFO overrun does not stall the DMA transfer.
14. SYSOSC FCL in internal-resistor mode (SysConfig permits it on G3x0x).
15. Buttons get the internal pull-up (harmless if the board also has one).
16. J-Link finds `_SEGGER_RTT` (scan or `-RTTAddress` from the ELF).

Residual timeline offsets (fixed per session): < 1 tick (250 ns) between the
two prescalers; ADC latency ≈ +5 µs for ECG and TIA_OUT, ≈ +15 µs for PPG_HI;
LED_BLANK rises at 9999.75 µs (one tick before the frame end).
