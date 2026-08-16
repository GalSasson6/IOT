# ESP32-2432S028 project notes

## Identified hardware

- Board: ESP32-2432S028 / ESP32-2432S028R, commonly called the Cheap Yellow Display (CYD)
- MCU module: ESP32-WROOM-32
- Screen: 2.8-inch, 240 x 320, ILI9341-compatible TFT
- Touch controller: XPT2046 resistive touch
- USB-to-serial controller: CH340
- Flash size used by the PlatformIO configuration: 4 MB

## Confirmed test status

The display test in `src/main.cpp` was compiled and uploaded successfully.
The board appeared as `COM7` during the test; the COM number can change when it
is connected again. Serial output at 115200 baud confirmed both startup lines:

```text
Starting ESP32-2432S028 display test...
Display test started successfully.
```

The expected screen output is the board name, `Display works!`,
`VS Code + PlatformIO`, and a moving red dot.

## LCD pin mapping used by this project

| Signal | ESP32 GPIO |
|---|---:|
| LCD MISO | 12 |
| LCD MOSI | 13 |
| LCD clock | 14 |
| LCD chip select | 15 |
| LCD data/command | 2 |
| LCD reset | Connected to board reset (`-1` in software) |
| Backlight | 21 |

## Other useful onboard pins

| Function | ESP32 GPIO |
|---|---:|
| Touch clock | 25 |
| Touch MOSI | 32 |
| Touch MISO | 39 |
| Touch chip select | 33 |
| Touch interrupt | 36 |
| RGB LED red | 4 |
| RGB LED green | 17 |
| RGB LED blue | 16 |
| Light sensor | 34 |
| Speaker/amplifier | 26 |

The touch controller uses a separate SPI bus from the LCD. Touch support is not
enabled in the current display-only sample.

## VS Code and PlatformIO

The PlatformIO IDE extension was installed in VS Code, and PlatformIO Core was
installed for command-line builds. Open this entire folder in VS Code so
PlatformIO detects `platformio.ini`.

Common commands from this folder:

```text
pio run
pio run --target upload
pio device monitor --baud 115200
```

If automatic port detection fails, find the current CH340 COM port and use:

```text
pio run --target upload --upload-port COM7
```

Replace `COM7` if Windows assigns a different number.

## USB connection

Use a USB data cable. On this dual-USB board revision, the small Micro-USB
connector is the reliable CH340 programming port; the USB-C connector may only
provide power. A lit board does not necessarily mean Windows has detected the
serial interface.

If upload remains at `Connecting...`, hold `BOOT`, tap `RST`, then release
`BOOT` when writing starts.

