# custom udp lcd

C++23 tools for custom media on the UniFi UDM Pro 240×240 LCD. Currently supports **UDM Pro only**. Other devices can be ported by implementing their MCU hooks, transport and verified flash/recovery paths; submit a PR to this repository.

FFmpeg prepares images, GIFs and videos as INDEX8 frames with an RGB565 palette. A static ARM64 runtime streams frames to the patched display MCU. Background playback runs under systemd on the UDM and continues after the PC disconnects; temporary uploads and the service do not survive a UDM reboot. FPS is a pacing limit, not guaranteed throughput.

## Compatibility

- UDM Pro (`UDMPRO`, system ID `ea15`, ARM64), with root SSH access.
- Display firmware `v2.5.7-0-gd26945b` and the exact supported stock or receiver image.
- Exact pinned `/usr/bin/ulcmd`, `/usr/bin/dfu-util` and `/lib/systemd/system/ulcmd.service` hashes, defined in [src/device.cpp](src/device.cpp).
- Internal USB path `1-2`, application VID:PID `1f9b:157d` with `/dev/ttyACM0`, and ROM DFU VID:PID `0483:df11` for flashing.

There is no hardcoded unit serial. The installer captures each device's DFU serial and pins it only for that flashing transaction. Other UDM Pros must pass the same compatibility checks; only one physical unit has been tested. Different firmware or hardware revisions may be rejected and require compatibility work. Use **Inspect device** to check eligibility.

## Build

Requires CMake, Ninja, pkg-config, a C++23 compiler, JsonCpp, LZ4, OpenSSL and SDL2. Media preparation uses FFmpeg/ffprobe; the file picker uses KDialog. Docker builds the ARM64 runtime; Clang and LLD build the MCU payload. Dear ImGui is fetched at the pinned commit in CMakeLists.txt.

```sh
./build.sh native
./build.sh aarch64
./build/native/udm-lcd-gui
```

## Playback

```sh
./build/native/udm-lcd play animation.gif --host root@192.168.1.1 --background
./build/native/udm-lcd status --host root@192.168.1.1
./build/native/udm-lcd stop --host root@192.168.1.1
```

The GUI provides media selection, session-only SSH credentials, device inspection, firmware actions and command output. Playback requires the matching receiver firmware. Authentication uses SSH; passwords are not saved.

## Firmware

Firmware images, media packs, build outputs and device records are excluded. Bootstrap requires the UDM Pro `v2.5.7-0-gd26945b` stock display image: 937,888 bytes, SHA-256 `933eeae931a276ac19b74711542408e840238721282c76a5ca59c01e045008b1`.

```sh
./build/native/udm-lcd firmware build --stock /path/to/stock.bin --output assets/firmware
./build/native/udm-lcd firmware inspect
```

The output directory must be `assets/firmware` for the GUI and deployment commands. Bootstrap reconstructs the pinned base/rollback images and builds the receiver and manifest. Installation and recovery validate model, revision, image hashes, USB/DFU identity and readback. Use the GUI Firmware tab, or `firmware install --host root@192.168.1.1 --live` / `firmware recover --host root@192.168.1.1 --live`.
