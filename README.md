# Astra918 SoapySDR support

This repository provides a generic SoapySDR receive driver for the Astra918. It
is a separate SoapySDR module; applications that use SoapySDR do not need to
recompile their own program. The driver and USB protocol are independent of any
one SDR application.

The module provides one receive channel with a 120 kS/s complex I/Q stream in
CS16 or CF32 format. It also exposes the receiver's RF input, RF and IF gain,
LF gain and attenuation, capacitor tuning, audio demodulator settings, clock
source, and logical GPIO values through standard SoapySDR controls and generic
device settings. A host's settings panel may choose which generic settings it
displays.

The receiver firmware remains authoritative. Frequency reads query its live
status, so a CAT or GUI tune is visible on the next SoapySDR frequency read.
The driver also polls status in the background at 5 Hz. SoapySDR has no general
event that forces every host application's display to recenter, so the host
must poll and decide how to update its display.

The documented receiver tuning range is 70 kHz to 130 MHz. The SoapySDR sample
rate is fixed at 120 kS/s and the RF filter bandwidth is reported from firmware.
No bandwidth resampler is part of this module.

## Build and install on Linux

The current build target is Linux x86-64. Other platforms have not been
validated yet.

Install the compiler, CMake, libusb, and SoapySDR development files. For Debian
or Ubuntu:

```sh
sudo apt install build-essential cmake libusb-1.0-0-dev libsoapysdr-dev soapysdr-tools
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build
```

To install in a user directory instead, use a prefix and set the plugin path
when launching the SoapySDR application:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
SOAPY_SDR_PLUGIN_PATH="$HOME/.local/lib/SoapySDR/modules0.8" SoapySDRUtil --info
```

The module is named `libSoapyAstra918Support.so`. SoapySDR normally discovers
it in its configured module directory. `SOAPY_SDR_PLUGIN_PATH` can point to a
different module directory for a local install.

## USB access and discovery

The driver opens only the Astra918 vendor interface 4. It does not claim the
USB audio or serial interfaces, reset the composite device, or change its USB
configuration. On a Linux desktop using systemd-logind, install a udev rule to
grant access to the logged-in user:

```sh
sudo install -m 0644 packaging/70-astra918.rules /etc/udev/rules.d/70-astra918.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug and reconnect the receiver, then check that SoapySDR sees it:

```sh
SoapySDRUtil --find="driver=astra918"
SoapySDRUtil --probe="driver=astra918"
```

If more than one Astra918 is connected, select a device by its USB serial:

```sh
SoapySDRUtil --probe="driver=astra918,serial=RECEIVER_SERIAL"
```

If the receiver has no USB serial descriptor, the driver reports a temporary
bus/address identity. Use the value printed by `--find` for that connection.

## Receiver controls

The standard SoapySDR API exposes spectrum-center tuning, the fixed sample
rate, the firmware-reported bandwidth, RF input selection, and RF/IF/LF gain
elements. Standard antenna choices are `Auto`, `LF`, `HF`, and `VHF`. Gain
values are reported in dB and mapped to the firmware's documented gain steps.

Device settings include RF/IF automatic or manual gain mode, raw gain codes,
the LF/MF capacitor code, firmware USB audio offset, USB/LSB mode, audio filter
edges, reference source, and Save/Retry actions. The offset setting preserves
the spectrum center; tuning the spectrum center preserves the current firmware
audio offset. Set `save=true` only when the current receiver settings should
be persisted in flash. Other changes take effect immediately and do not write
flash.

The clock-source options appear only when firmware status advertises reference
selection. GPIO0 through GPIO7 and the SoapySDR `GPIO` bank appear only when
firmware advertises logical GPIO support. These are stored logical values;
the RP2350 firmware does not assign them to physical pins. GPIO direction calls
are therefore unsupported.

Use one host as the owner of the receiver's vendor control interface at a time.
Other programs can continue to use the separate firmware audio/CAT USB
interfaces. CAT and this module share firmware state, including the dial and
audio offset.

## Offline checks

Build and run the mock-transport tests without an Astra918 attached:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The mock tests cover control framing, status validation, fragmented I/Q frames,
sample conversion, tuning and offset preservation, external CAT frequency
readback, feature-gated settings, explicit flash save, and stream lifecycle.
They do not perform hardware or frequency-generator tests.

## Protocol source

The AST1 control and ASIQ stream formats are documented in the
[Astra918 firmware protocol](https://github.com/ur8us/astra918sdr/blob/main/docs/PROTOCOL.md).
This module follows that USB protocol and is licensed under MIT.
