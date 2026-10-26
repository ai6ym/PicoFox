# PicoFox – A Hackable 2m Fox Transmitter

**PicoFox** is an open source fox transmitter for the 2-meter amateur band, built around the
RP2040 microcontroller (the same chip used in the Raspberry Pi Pico). It’s designed to be simple,
functional, and easy to modify.

PicoFox works well as a standalone fox for portable or walking hunts, but it’s also a great platform
for experimentation. There's ample CPU, flash, and GPIO available if you want to add features like
sensors or interactivity. The transmitter can generate a frequency or phase modulated signal with up
to 2.5kHz (data) bandwidth.


## Key Features
- Transmits FM in the 144–148 MHz band (ITU Zone 1: 144–146 MHz).
- Configured by editing a text file — no software or terminal needed.
- Transmits any 5kHz 16-bit PCM WAV file.
- Automatic Morse code ID with configurable tone, speed, and spacing.
- Scheduled start and stop times with low-power sleep before and after a hunt.
- Percentage-based or fixed-time transmit cycles.
- 15 MB user volume for custom audio and configuration files.
- RF output > 18dBm at full power. Down to ~ -20dBm with full attenuation.
- Charges over USB.
- Expansion header provides:
  - 12 GPIOs (including I2C, SPI, UART, ADC).
  - Regulated 3.3V and raw battery voltage.
  - Two additional SI5351 clock outputs


### Getting One

Fully assembled PicoFox units are available from [AI6YM.radio](https://ai6ym.radio/picofox). An
enclosure, battery, and antenna are included. I ship nearly anywhere in the world.

You can also build your own - both the hardware and software are open source for non-commercial use.
For commercial inquiries, contact [justin@ai6ym.radio](mailto:justin@ai6ym.radio).


## Configuration

To configure your PicoFox, connect it to any computer using a USB-C cable. It will appear as a
USB mass storage device (like a flash drive). Open `settings.txt` with a text editor and adjust the
configuration values as needed. Eject the drive, disconnect USB, and power cycle the PicoFox to
apply the new settings.

To reset to factory settings, delete `settings.txt`, eject the drive, disconnect USB, and power
cycle the PicoFox. A default settings file will be regenerated.

**Note:** if the PicoFox is powered on but does not appear as a USB storage device when connected
over USB there is a 99% chance you have a bad cable. The current record is three bad cables in a row
before a good cable was found. The world is full of bad cables and cables built for power only with
no data wires.

This document applies to PicoFox r3 - all PicoFox orders from January 2026 on are r3 hardware. The
r2 PicoFox had a lower output power (15mW / 12dBm) and did not include the in-series attenuator (so
the ATTENUATION configuration field does nothing). It used a micro USB connector instead of USB-C
and could not charge while off. Otherwise the information in this document applies.


### Settings Overview

These settings reflect the current firmware version (v2.1). If your `settings.txt` file does not
contain all of these settings that indicates you are running an older firmware version.

- `CALLSIGN`: Alphanumeric callsign (max 12 characters).
- `ITU_ZONE`: ITU zone where the transmitter operates (`1`, `2`, or `3`).
- `FREQ_MHZ`: Transmit frequency in MHz.
- `ATTENUATION`: Attenuation level `0`–`151`, approximately 0.25dB steps.
- `DUTY_CYCLE`: Transmit duty cycle percentage (`1`–`100`), or `0` to use the fixed-time cycle
  settings.
- `INITIAL_SLEEP_SECS`: Seconds to remain in low-power sleep after startup and before the first
  transmission (`0` starts immediately).
- `RUNTIME_SECS`: Seconds to run transmit cycles after the initial sleep (`0` runs indefinitely).
- `CYCLE_ON_SECS`: Transmit time per cycle when `DUTY_CYCLE=0`.
- `CYCLE_OFF_SECS`: Sleep time between transmissions when `DUTY_CYCLE=0`.
- `MORSE_WPM`: Morse ID speed in words per minute.
- `MORSE_FARNSWORTH_WPM`: Slower Farnsworth spacing speed (ignored if lower than `MORSE_WPM`).
- `MORSE_TONE`: Morse tone frequency (100–2000 Hz).
- `MORSE_TONE_VOL`: Morse tone volume percentage (`1–100`).

**Note:** Invalid or missing values may disable the transmitter or revert to safe defaults.


### Scheduling and Duty Cycle

All timing values are whole seconds and support at least a full day (`86400`). The default remains
continuous transmission at 100% duty cycle, starting immediately and running indefinitely. The
60-second fixed-cycle values are present but do not take effect unless `DUTY_CYCLE` is changed to
`0`:

```text
DUTY_CYCLE=100
INITIAL_SLEEP_SECS=0
RUNTIME_SECS=0
CYCLE_ON_SECS=60
CYCLE_OFF_SECS=60
```

`INITIAL_SLEEP_SECS` provides time to hide the fox before it starts transmitting. The
radio output remains off and the processor uses low-power sleep during this delay. After the delay,
`RUNTIME_SECS` starts counting. When a nonzero runtime expires, PicoFox stops the normal cycle,
transmits its CW callsign ID one final time, switches the RF output off, and returns to indefinite
low-power sleep. The final ID is always the last transmission, regardless of the other cycle
settings.

For the original percentage-based behavior, set `DUTY_CYCLE` from `1` to `100`. Each transmission
contains `audio.wav` followed by the CW ID; the firmware calculates the off time needed to produce
the selected percentage.

For exact on/off times, set `DUTY_CYCLE=0`. PicoFox repeats or trims `audio.wav` to fill
`CYCLE_ON_SECS`, reserves the end of that interval for the CW ID, then sleeps for
`CYCLE_OFF_SECS`. If the configured on time is too short to contain the complete ID, the firmware
raises `CYCLE_ON_SECS` to the required minimum and writes the corrected value back to
`settings.txt`.

For example, the following waits two hours, transmits in 60-second on/120-second off cycles for
three hours, sends a final CW ID, and sleeps until the unit is collected:

```text
DUTY_CYCLE=0
INITIAL_SLEEP_SECS=7200
RUNTIME_SECS=10800
CYCLE_ON_SECS=60
CYCLE_OFF_SECS=120
```


## Using the Transmitter

After configuring settings as described above, the transmitter will turn on following the
provided settings the next time it is turned on. The transmitter may be powered by an external USB
power supply or the internal battery.

A full battery will last about ten hours at 100% duty cycle.


## Updating the Firmware

Most PicoFox devices in the wild are running firmware v1, this is true for both r2 and r3 hardware.
The firmware is the same for either hardware version but some features will not work on r2.

You can identify your firmware version by looking at the `settings.txt` file. If the file contains
the `INITIAL_SLEEP_SECS` setting you are running v2 or later; if not, you are running v1.


### v2 and Later Easy Update Method

Firmware v2 and later supports entering the bootloader without opening the enclosure or holding the
physical `BOOTSEL` button:

1. Back up `settings.txt` and `audio.wav` before updating. The update normally preserves the user
   volume, but keeping a copy protects your custom configuration and audio.
2. Create an empty file named exactly `BOOTSEL` (with no extension) in the root of the `PICOFOX`
   USB drive.
3. Eject the `PICOFOX` drive safely, then reset or power cycle the PicoFox.
4. Wait for the RP2040 bootloader drive named `RPI-RP2` to appear.
5. Copy [`releases/pfoxr3v2.1.uf2`](releases/pfoxr3v2.1.uf2) to `RPI-RP2`. Do not copy the UF2 to the
   normal `PICOFOX` drive.
6. The board will reboot automatically. When `PICOFOX` reappears, restore or edit your settings and
   audio as needed, eject it safely, and power cycle once more to apply them.

The firmware removes the `BOOTSEL` marker before entering the bootloader, so later boots return to
normal operation.


### v1 Serial Touch Method

Older firmware does not recognize the `BOOTSEL` marker file, but it can still enter programming
mode through its USB serial port. Nothing needs to be typed into the console: opening the port at
1200 baud and then closing it tells the PicoFox to start its bootloader.

Before starting, close any files on the PicoFox and wait for file copies to finish. Use a
USB cable that supports data, not a charge-only cable. Use one of the methods below according to the
operating system you are using.

Once the PicoFox is in programming mode a drive named `RPI-RP2` should appear. Copy
[`releases/pfoxr3v2.1.uf2`](releases/pfoxr3v2.1.uf2) to that drive; the PicoFox will reboot
automatically when the copy finishes.

If this method does not work, or you prefer not to mess with serial consoles, you may use the
physical buttons on the board as described in the next section.

#### Linux

1. Connect the PicoFox and find its serial port:

   ```bash
   ls /dev/ttyACM*
   ```

2. Run the following command, replacing `/dev/ttyACM0` if a different port was listed:

   ```bash
   stty -F /dev/ttyACM0 1200
   ```

   If the command reports "permission denied," run it again with `sudo`.

#### macOS

1. Connect the PicoFox and find its serial port:

   ```bash
   ls /dev/cu.usbmodem*
   ```

2. Run the following command using the complete port name reported above:

   ```bash
   stty -f /dev/cu.usbmodemXXXX 1200
   ```

#### Windows

1. Connect the PicoFox.
2. Open **Device Manager**, expand **Ports (COM & LPT)**, and note the PicoFox port, such as
   `COM5`.
3. Open PowerShell and run the following commands, replacing `COM5` with the correct port:

   ```powershell
   $p = [System.IO.Ports.SerialPort]::new()
   $p.PortName = "COM5"
   $p.BaudRate = 1200
   $p.DtrEnable = $true
   $p.Open()
   Start-Sleep -Milliseconds 100
   $p.DtrEnable = $false
   $p.Dispose()
   ```

**Note:** I offer no support for this method. Windows is a terrible environment for any sort of
embedded programming or development work of any kind.


### Physical Button Method

Open the PicoFox enclosure and find the buttons on the board. One is labelled `RESET` and the other
`BOOTSEL`. With the PicoFox connected to your computer and powered on hold the `BOOTSEL` button,
press and release the `RESET` button, then release the `BOOTSEL` button. A drive labelled `RPI-RP2`
should appear. Copy [`releases/pfoxr3v2.1.uf2`](releases/pfoxr3v2.1.uf2) to that drive; the PicoFox
will reboot automatically when the copy finishes.


## Replacing the Audio File

To change the audio transmission, replace the `audio.wav` file in the device’s flash storage. The
audio must be sampled at **5kHz**, **mono** (one channel), and encoded as **signed 16-bit PCM WAV**.

Using **Audacity** (free software available for all major OSs):

1. Set "Project Rate (Hz)" to `5000`.
2. Import, record, or generate your audio.
3. Mix down to a single mono track.
4. Export as WAV using **Signed 16-bit PCM** encoding.
5. Name the file `audio.wav` and copy it to the device.
6. Eject the drive and disconnect USB.
7. Power cycle the PicoFox.

**Notes:**

- Improper format may prevent audio transmission or cause noise to be transmitted instead.
- The audio is transmitted, followed by the morse ID, and then the transmitter turns off if
  `DUTY_CYCLE < 100`. In this way the length of the audio determines the transmit cycle time and the
  frequency of morse code ID transmissions. **YOU ARE RESPONSIBLE FOR COMPLYING WITH LOCAL LAWS FOR
  TRANSMITTING YOUR CALLSIGN!**
- To restore the default audio, delete `audio.wav`, eject the drive, disconnect USB, and power cycle
  the PicoFox.


## Design Brief

An SI5351 clock generator, locked to a TCXO, provides the RF signal. Modulation is applied in
software by the RP2040. To achieve smooth FM output, modulation is handled on core 1 with
I<sup>2</sup>C running at 1 MHz. Register writes are minimized and no other devices are on
I<sup>2</sup>C0.

Core 0 manages flash operations. Most extensions or new features should run on core 0. The latest
firmware uses a two core buffer approach to reduce the clock speed required and so reduce power
consumption. You may need to raise the clock speed to support any features you add.

**Note:** Mounting the device as a USB mass storage device will interrupt clean modulation as both
cores are blocked during flash operations.

The SPI flash device is split into program memory and a 15 MB FAT12 filesystem. The filesystem
contains a text file for user-supplied settings, an audio file for transmission, a second audio
file containing the generated morse code ID, and a hash file used to detect changes to the settings
file. Missing user files are re-created from defaults in the firmware binary. A damaged filesystem
is reformatted automatically as FAT12 before the default files are restored.

The design intentionally includes large amounts of free CPU cycles, RAM, and flash so that cool
things can be built on the PicoFox.


## Development Guide

### Build Environment Setup

- Install Arduino IDE >= 2.3.
- Install libraries - Adafruit SPIFlash, Adafruit TinyUSB, Etherkit Si5351.
- Install board package from [my fork of Arduino-Pico](https://github.com/ai6ym/arduino-pico). This
  will be merged upstream eventually but for now you'll need the changes in my fork.
- In `Tools -> Board` select the `Raspberry Pi RP2040/RP2350 Boards -> AI6YM PicoFox`.
- Connect your PicoFox and select the appropriate TTY / COM port.

On Linux it may be necessary to add udev rules for this device. The relevant rules from my system
are below. I have no idea how this works on other operating systems. Mac is probably similar,
Windows should not be used for software developement of any kind (or really anything).

```
# Make an RP2040 in BOOTSEL mode writable by all users, so you can `picotool`
# without `sudo`. 
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="0003", MODE="0666"

# Symlink an RP2040 running MicroPython from /dev/pico.
#
# Then you can `mpr connect $(realpath /dev/pico)`.
SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="0005", SYMLINK+="pico"

# Adafruit TinyUSB stack.
SUBSYSTEM=="tty", ATTRS{idVendor}=="239a", ATTRS{idProduct}=="cafe", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="239a", ATTRS{idProduct}=="cafe", MODE="0666"
```

### Compilation Settings

- `Flash Size`: `16MB (Sketch: 1MB, FS: 15MB)`
- `CPU Speed`: `200 MHz`
- `USB Stack`: `Adafruit TinyUSB`

Do not select a different partition layout for a release build. The firmware expects the 15 MB
filesystem layout; another choice can hide or overwrite the user volume. If the expected filesystem
is missing or damaged, the firmware formats a new FAT12 volume and writes defaults. The current
firmware uses about 697 KB of the 1 MB sketch partition.

All other settings are fine at default values.


### Recovering a Board

In general the UF2 upload process will work fine. Click the upload button, after compilation the IDE
will push the board into UF2 update mode, write the new binary, and then reset the board to normal
operation.

If that doesn't work try the following:
- Use the `BOOTSEL` marker-file method described in [Updating the Firmware](#updating-the-firmware).
- For firmware that does not support the marker file, use the
  [USB serial method](#updating-older-firmware-over-usb-serial).
- Reset the board and try again. Just tap the `RESET` button on the board to reset the RP2040.
- Force the device into `BOOTSEL` mode. Hold the `BOOTSEL` button, tap the `RESET` button, and then
  release the `BOOTSEL` button.
- Force the device into `BOOTSEL` mode **AND** change `Tools -> Upload Method` to `PicoTool`.
- When all else fails grab a PicoProbe (or a Pico / Pico2 flashed with the PicoProbe binary),
  connect that to the debug header of the PicoFox, and change `Tools -> Upload Method` to
  `Picoprobe`.
- If all of the above fails, well, he's dead Jim.
