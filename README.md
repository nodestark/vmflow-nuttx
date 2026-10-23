# VMflow NuttX

NuttX port of the [VMflow](https://github.com/nodestark/mdb-esp32-cashless) MDB cashless
firmware for the ESP32-S3.

> **Status** — bring-up. The image boots NuttX with an NSH console on the chip's native
> USB port (USB-Serial/JTAG) and ships the `vmflow` application skeleton. MDB, Wi-Fi/LTE uplink,
> MQTT RPC, BLE and OTA from the ESP-IDF firmware are not ported yet.

## Layout

```
.
├── nuttx/                    apache/nuttx      (submodule, nuttx-13.1.0)
├── apps/                     apache/nuttx-apps (submodule, nuttx-13.1.0)
├── firmware/
│   ├── apps/                 VMflow applications, linked as apps/external
│   │   └── vmflow/           cashless application (`vmflow` NSH command)
│   └── configs/
│       ├── vmflow/           default: NSH on USB-Serial/JTAG (on top of esp32s3-devkit)
│       └── vmflow-otg/       alternative: NSH on USB-OTG CDC-ACM
├── docker/Dockerfile         build environment (xtensa-esp-elf 14.2.0, esptool 5.2.0, kconfig)
└── Makefile                  build, flash and monitor through Docker
```

## Build

Requires Docker. Everything else lives in the image.

```bash
git clone --recurse-submodules <repo-url> vmflow-nuttx
cd vmflow-nuttx

make image       # toolchain image (once)
make configure   # apply firmware/configs/vmflow
make build       # -> nuttx/nuttx.bin
```

The first build clones Espressif's `esp-hal-3rdparty` into the NuttX tree, so it needs
network access.

Behind a TLS-inspecting proxy, `make image` forwards the host CA store
(`/usr/local/share/ca-certificates`) into the image. Point `EXTRA_CA_DIR` elsewhere if
your certificates live in another directory.

## Flash and console

```bash
make flash               # PORT=/dev/ttyACM0 by default
make monitor             # NSH console, exit with Ctrl+]
```

```
nsh> vmflow
VMflow on NuttX 13.1.0 (xtensa)
build: 2b5c0de5 Oct 23 2026 13:11:50
nsh> uname -a
NuttX 13.1.0 2b5c0de5 Oct 23 2026 13:11:50 xtensa esp32s3-devkit
```

Flashing and the console share the chip's native USB port (GPIO19/20), which shows up as
`303a:1001` Espressif USB JTAG/serial in both download mode and while NuttX runs.

On the USB-Serial/JTAG, DTR and RTS drive the chip's BOOT and RESET lines. `make monitor`
keeps both low; a terminal that asserts them on open holds the chip in reset or drops it
into download mode.

### USB-OTG console (`vmflow-otg`)

```bash
make configure build flash CONFIG=vmflow-otg
```

The console moves to the USB-OTG peripheral as a CDC-ACM device (`0525:a4a7` NuttX
CDC/ACM, marked experimental in NuttX). The port changes identity when NuttX starts, so
esptool can no longer reach the chip: to flash again, hold **BOOT** while plugging in. On a
VM, pass both USB IDs through.

## Configuration

```bash
make menuconfig      # edit nuttx/.config
make savedefconfig   # store it back into firmware/configs/vmflow/defconfig
```

## License

[MIT](LICENSE) for the VMflow code. NuttX and nuttx-apps are Apache-2.0.
