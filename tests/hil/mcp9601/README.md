# MCP9601 I²C recovery HIL experiment

**Research-only fixture, not production recovery code.** This suite measures the MCP9601's response to ordinary scanning and candidate recovery sequences. It deliberately **does not** exempt the MCP9601 from scans.

## Currently wired (stage 0)

* ESP32-S31: sole I²C master, running the independent ESP-IDF firmware in `esp32s31/`.
* MCP9601: only device on this test bus; its 7-bit address is configurable (0x67 default).
* Observe physical test-bus SDA/SCL using **two separate high-impedance S31 GPIO inputs** connected in parallel to those bus wires. RMT RX is assigned only to these dedicated sniff pins.
* **Do not reassign or touch the S31's existing analogue-mux I²C pins**, and do not connect another bus master without first physically isolating or disabling the S31 master.
* All grounds common; verify 3.3 V levels and pull-ups. No output drive from sniff pins.

GPIO selection is deliberately left unconfigured by default. Set the **test-bus** I²C pins, independent sniff pins and bus port under `idf.py menuconfig → MCP9601 HIL`. The firmware refuses to start if pins are missing or overlap, and never probes or pulses anything automatically at boot.

## Build / operate

Use an ESP-IDF checkout that supports `esp32s31` (currently IDF master/preview):

```sh
cd tests/hil/mcp9601/esp32s31
idf.py set-target esp32s31
idf.py menuconfig
idf.py build
idf.py -p <SERIAL_PORT> flash monitor
```

The serial console accepts commands: `help`, `id`, `scan`, `probe50`, `probemcp`, `captureid`, `capture50`, `wait10`, `reinit`, `stop`, `pulse1`, `pulse9`, `pulse18`, `levels`.

Every risky intervention is **manual**. `stop` and `pulse*` temporarily relinquish the test I²C controller and operate **only the configured test-bus pins**, never the analogue mux. Operate only when the S31 is the sole master; later muxed masters must be disconnected/tri-stated. Software delays are not a substitute for verifying a valid electrical STOP: record SCL and SDA.

`capture*` emits RMT pulse durations for both wires. RMT RX channels have separate first-edge time origins. Thus the trace is a useful per-line edge capture **but is not yet a synchronised two-channel protocol decode**. Use Picoprobe or a logic analyser to verify absolute alignment before interpreting precise START/ACK setup times. Very long transfers may overrun available RMT memory: start by capturing a single address probe.

## Separate trials — do not chain recovery mechanisms

1. Power-cycle *only* the MCP9601 or re-establish a verified healthy baseline. Run `id`; confirm the high Device ID byte is 0x41. Log silicon revision, VDD, frequency, board and firmware versions.
2. Run `capture50` and `id`, then `scan` and `id` in separate fresh-baseline trials. Probing 0x50 must happen **even though it is not populated**, to reproduce the reported RP2350 EDID case.
3. A successful scan or a simple NACK is **not** a lockup. Classify an experiment as `high_high_unresponsive` only when a valid MCP9601 ID register read fails afterwards *and* both physical bus wires are observed HIGH at rest; preserve the capture and exact error.
4. On a fresh reproduction for **each** mechanism: `wait10`, `reinit`, `stop`, `pulse1`, `pulse9`, `pulse18`, or `probe50` (ordinary transaction recovery), followed by `id`. Do not treat controller reinit as nine SCL pulses.
5. Record whether the S31's RMT capture agrees with external Picoprobe, especially for short START/STOP-only sequences and failed NACK probes.

No test may automatically declare the MCP9601 fixed based just on SDA/SCL HIGH. A post-operation valid ID read is required.

## Later hardware matrix

* RP2350 with Picoprobe: Arduino-Pico and CircuitPython RP2350 ports; include the board.I2C() deinit/recreate case and comparison to bare START/STOP with no SCL clock.
* Espressif DevKit: Arduino and CircuitPython ESP32 ports; controller deinit/reinit and probe implementation are expected to differ.
* Linux SBC (Raspberry Pi / Tachyon): Blinka and native CPython. Kernel owns I²C pins. Prioritise `/dev/i2c-*` probe/read and close/reopen; try `I2C_M_IGNORE_NAK` or `I2C_M_NOSTART` **only if the adapter advertises capability**, in an isolated trial.
* Add one independent, known-address I²C sensor to prove other devices still operate and to detect unintended register changes during recovery. Do not assume any chosen device accepts arbitrary reads.

Refer to `test_matrix.json` for explicit trial IDs, fixture readiness and acceptance evidence.

## Important source links

* [CircuitPython MCP9601 issue](https://github.com/adafruit/circuitpython/issues/11203)
* [RP2350 I²C implementation](https://github.com/adafruit/circuitpython/blob/main/ports/raspberrypi/common-hal/busio/I2C.c)
* [CircuitPython software I²C helper](https://github.com/adafruit/circuitpython/blob/main/shared-module/bitbangio/I2C.c)
* [ESP-IDF I²C API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/peripherals/i2c.html)
* [ESP-IDF RMT RX API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/peripherals/rmt.html)
* [MCP960x datasheet](https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/MCP960X-Data-Sheet-20005426.pdf)

### Non-goals

No watchdog or 10-second self-reset is documented by Microchip; the ten-second test is a negative control. Bare clocks + STOP are **not** interchangeable with normal 9-clock addressed probes. The tests will never omit the MCP9601 address from a full scan.
