# VMflow NuttX

NuttX port of the [VMflow](https://github.com/nodestark/mdb-esp32-cashless) MDB cashless
firmware for the ESP32-S3.

> **Status** — bring-up. The image boots NuttX with an NSH console on the chip's native
> USB-OTG port (CDC-ACM) and ships the `vmflow` application skeleton. MDB, Wi-Fi/LTE uplink,
> MQTT RPC, BLE and OTA from the ESP-IDF firmware are not ported yet.

## Layout

```
.
├── nuttx/                    apache/nuttx      (submodule, nuttx-13.1.0)
├── apps/                     apache/nuttx-apps (submodule, nuttx-13.1.0)
├── firmware/
│   ├── apps/                 VMflow applications, linked as apps/external
│   │   └── vmflow/           cashless application (`vmflow` NSH command)
│   └── configs/vmflow/       board configuration (defconfig on top of esp32s3-devkit)
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
make monitor             # NSH on the USB-OTG CDC-ACM port
```

```
nsh> vmflow
VMflow on NuttX 13.1.0 (esp32s3-devkit)
```

The console runs on the USB-OTG peripheral (GPIO19/20), which shares the PHY with the
USB-Serial/JTAG controller used for flashing:

| State | USB ID | Role |
|-------|--------|------|
| Download mode / ROM | `303a:1001` Espressif USB JTAG/serial | flashing with esptool |
| NuttX running | `0525:a4a7` NuttX CDC/ACM | NSH console |

Once NuttX is running, esptool can no longer reset the chip over USB. To flash again,
hold **BOOT**, tap **RESET** (or replug), release **BOOT**, then run `make flash`.

## Configuration

```bash
make menuconfig      # edit nuttx/.config
make savedefconfig   # store it back into firmware/configs/vmflow/defconfig
```

## License

[MIT](LICENSE) for the VMflow code. NuttX and nuttx-apps are Apache-2.0.
