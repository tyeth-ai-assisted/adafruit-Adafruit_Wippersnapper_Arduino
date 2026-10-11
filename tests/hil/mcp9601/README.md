# MCP9601 I²C recovery HIL experiment

**Research-only fixture, not production recovery code.** This suite measures the MCP9601's response to ordinary scanning and candidate recovery sequences. It deliberately **does not** exempt the MCP9601 from scans.

## Currently wired (stage 0)

* ESP32-S31: sole I²C master, running the independent ESP-IDF firmware in `esp32s31/`.
* MCP9601: only device on this test bus; its 7-bit address is configurable (0x67 default).
* Observe physical test-bus SDA/SCL using **two separate high-impedance S31 GPIO inputs** connected in parallel to those bus wires. The first two allocated RMT RX channels are routed to GPIO42 and GPIO43 respectively (software-allocated channels; S31 has no fixed RMT-only GPIO bank).
* **Do not reassign or touch the S31's existing analogue-mux I²C pins**, and do not connect another bus master without first physically isolating or disabling the S31 master.
* All grounds common; verify 3.3 V levels and pull-ups. No output drive from sniff pins.

The S31 I²C pin defaults are now **SDA GPIO2 / SCL GPIO3** (as supplied for Arduino-ESP32 4.x / ESP-IDF 6.x); the separate logic capture defaults are **GPIO42 (SCL observer) / GPIO43 (SDA observer)**. These are non-SPI GPIOs that are also ADC inputs; they are assigned here only as high-impedance RMT RX inputs. If using Espressif Function-CoreBoard-1, GPIO42 is J2 pin 20 and GPIO43 is J2 pin 17; on another S31 board, confirm the header mapping from its schematic.

**Physical loopbacks needed:** bridge test-bus GPIO3 (SCL) to observer GPIO42; bridge test-bus GPIO2 (SDA) to observer GPIO43. Do not accidentally connect a logic analyser input to an unrelated header labelled "LA0" or "LA1" without checking its actual GPIO mapping. The inputs must be 3.3 V compatible, floating pull-ups must not be supplied by the observer wires, and GPIO42/43 must not already be used by an analogue function or other peripheral.

Override defaults under `idf.py menuconfig → MCP9601 HIL` if the actual board has conflicts. The firmware still refuses duplicate/invalid GPIOs and does not run probes or recovery at boot. In ESP-IDF, RMT receiver channels are dynamically allocated; the first two requested logical captures are SCL and SDA.

## Build / operate

Use an ESP-IDF checkout that supports `esp32s31` (ESP-IDF 6.1 stable or newer):

```sh
cd tests/hil/mcp9601/esp32s31
idf.py set-target esp32s31
idf.py menuconfig
idf.py build
idf.py -p <SERIAL_PORT> flash monitor
```

The serial console accepts commands: `help`, `id`, `scan`, `probe50`, `probemcp`, `captureid`, `capture50`, `wait10`, `reinit`, `stop`, `pulse1`, `pulse9`, `pulse18`, `levels`, `wiring`.

Before fault injection, verify the two loopbacks by explicitly running `wiring`: it temporarily releases the test I²C controller, pulls SCL LOW, toggles SDA LOW/HIGH **while SCL is LOW**, then releases SCL and checks that GPIO42/43 see each change. It prints a PASS/FAIL for each observation; this is **not** a two-channel RMT timestamp-alignment check, and it does add one test SCL excursion. Always run a valid `id` after this preflight and establish a clean baseline before measuring failure.

Every risky intervention is **manual**. `stop` and `pulse*` temporarily relinquish the test I²C controller and operate **only the configured test-bus pins**, never the analogue mux. Operate only when the S31 is the sole master; later muxed masters must be disconnected/tri-stated. Software delays are not a substitute for verifying a valid electrical STOP: record SCL and SDA.

`capture*` emits RMT pulse durations for both wires. RMT RX channels have separate first-edge time origins. Thus the trace is a useful per-line edge capture **but is not yet a synchronised two-channel protocol decode**. Use Picoprobe or a logic analyser to verify absolute alignment before interpreting precise START/ACK setup times. Very long transfers may overrun available RMT memory: start by capturing a single address probe.

## Raspberry Pi USB serial host (Windows OpenSSH)

This Pi can be accessed using the already-configured Windows OpenSSH client; **no SSH private keys belong in this repository or the test logs**. There is no direct SSH/serial execution service attached to this chat, so the following steps are for a Windows shell which already has an authorised host alias or destination.

On the Windows machine (PowerShell), first identify the USB serial device exposed on the Pi:

~~~powershell
& "$env:WINDIR\\System32\\OpenSSH\\ssh.exe" -o BatchMode=yes <pi-ssh-host> 'ls -l /dev/serial/by-id/ 2>/dev/null; ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null; id'
~~~

On the Pi, from a working tree containing this branch, install python3-serial if needed and identify a stable USB path. **Do not assume the S31 is /dev/ttyACM0**: this Pi has multiple USB devices connected to its hub.

~~~sh
sudo apt install python3-serial
python3 tests/hil/mcp9601/pi_serial.py list
python3 tests/hil/mcp9601/pi_serial.py listen --port /dev/serial/by-id/<MATCH-S31> --duration 4
~~~

The helper never opens a serial port while listing. For other actions the port must be explicit:

~~~sh
python3 tests/hil/mcp9601/pi_serial.py wiring --port /dev/serial/by-id/<MATCH-S31> --allow-io --jsonl /tmp/mcp9601-preflight.jsonl
python3 tests/hil/mcp9601/pi_serial.py id --port /dev/serial/by-id/<MATCH-S31> --allow-io --jsonl /tmp/mcp9601-baseline.jsonl
python3 tests/hil/mcp9601/pi_serial.py capture50 --port /dev/serial/by-id/<MATCH-S31> --allow-io --duration 7 --jsonl /tmp/mcp9601-probe50.jsonl
~~~

USB-UART DTR/RTS can reset some Espressif dev boards on serial-open, which **is not equivalent to power-cycling the MCP9601** but does reset the test controller. The helper preconfigures DTR/RTS inactive before opening, but this should be checked for the chosen board. Port ownership may also require membership in the Pi's \`dialout\` group.

The Pi-side helper is only a serial front-end. It does not build or flash the uncompiled S31 firmware. Firmware compilation and first capture remain separate validation tasks. The initial RMT implementation has not been electrically verified.

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
