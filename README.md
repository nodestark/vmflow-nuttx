# VMflow NuttX

NuttX port of the [VMflow](https://github.com/nodestark/mdb-esp32-cashless) MDB cashless
firmware for the ESP32-S3.

> **Status** — bring-up. NuttX boots with an NSH console on the chip's native USB port
> (USB-Serial/JTAG). The SIM7080G comes up as a regular network interface (`ppp0`) through
> pppd, so sockets, DNS and HTTP go over LTE-M/NB-IoT. The `vmflow` application is still a
> skeleton: MDB, Wi-Fi, MQTT RPC, BLE and OTA from the ESP-IDF firmware are not ported yet.

## Layout

```
.
├── nuttx/                    apache/nuttx      (submodule, nuttx-13.1.0)
├── apps/                     apache/nuttx-apps (submodule, nuttx-13.1.0)
├── firmware/
│   ├── apps/                 VMflow applications, linked as apps/external
│   │   ├── vmflow/           cashless application (`vmflow` NSH command)
│   │   ├── sim7080g/         modem daemon: power, pppd, link supervision
│   │   └── rgbled/           `rgbled` command for the WS2812 status LED
│   └── boards/vmflow-esp32s3/
│       ├── src/              board code (derived from esp32s3-devkit), PWRKEY GPIO, RGB LED
│       └── configs/
│           ├── vmflow/       default: NSH on USB-Serial/JTAG, SIM7080G + PPP
│           └── vmflow-otg/   alternative: NSH on USB-OTG CDC-ACM
├── docker/Dockerfile         build environment (xtensa-esp-elf 14.2.0, esptool 5.2.0, kconfig)
└── Makefile                  build, flash and monitor through Docker
```

## Build

Requires Docker. Everything else lives in the image.

```bash
git clone --recurse-submodules --shallow-submodules https://github.com/nodestark/vmflow-nuttx
cd vmflow-nuttx

make image       # toolchain image (once)
make configure   # apply firmware/boards/vmflow-esp32s3/configs/vmflow
make build       # -> nuttx/nuttx.bin
```

`--shallow-submodules` fetches only the pinned NuttX snapshot (~300 MB of git data) instead of the full
history. For a clone made without it:

```bash
git submodule update --init --depth 1
```

The first build clones Espressif's `esp-hal-3rdparty` into the NuttX tree, so it needs
network access.

Behind a TLS-inspecting proxy, `make image` forwards the host CA store
(`/usr/local/share/ca-certificates`) into the image. Point `EXTRA_CA_DIR` elsewhere if
your certificates live in another directory.

## Flash and console

```bash
make flash               # PORT=/dev/ttyACM0 by default
make monitor             # NSH console, exit with Ctrl+] (close it before flashing)
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
into download mode. Linux may still pulse them when the port opens, which resets the board;
the modem service comes back up on its own.

### USB-OTG console (`vmflow-otg`)

```bash
make configure build flash CONFIG=vmflow-otg
```

The console moves to the USB-OTG peripheral as a CDC-ACM device (`0525:a4a7` NuttX
CDC/ACM, marked experimental in NuttX). The port changes identity when NuttX starts, so
esptool can no longer reach the chip: to flash again, hold **BOOT** while plugging in. On a
VM, pass both USB IDs through.

## SIM7080G (LTE-M / NB-IoT)

The modem is a boot service: nxinit starts `sim7080g` from `/etc/init.d/init.rc`
(`firmware/boards/vmflow-esp32s3/src/etc/init.d/init.rc`) next to the console, and `ppp0`
is up about 40 s after power-on with no command typed. Applications just open sockets.

```
nsh> ps
    4     4     0 100 RR       Task      - Waiting  Semaphore ... init_main
    5     5     4 100 RR       Task      - Running            ... sh
    6     6     4 100 RR       Task      - Waiting  Signal    ... sim7080g
    7     7     6 100 RR       Task      - Waiting  Semaphore ... pppd
nsh> sim7080g status
daemon  : pid 6, pppd pid 7
link    : up for 46 s
ip      : 100.82.56.6
apn     : lf.br, cmnb 3, tty /dev/ttyS1
nsh> nslookup mqtt.vmflow.xyz
Host: mqtt.vmflow.xyz Addr: 144.202.18.100
nsh> ping -c 2 google.com
56 bytes from 142.251.132.238: icmp_seq=0 time=190.0 ms
```

```
 app (MQTT, HTTP, OTA)    socket() as usual
 NuttX TCP/IP             routes, DNS
 ppp0                     TUN netdev (nuttx/net/tun.c)
 pppd                     apps/netutils/pppd: AHDLC, LCP, PAP, IPCP
 /dev/ttyS1               UART1, GPIO17 TX / GPIO18 RX, 115200
```

| Command | Use |
|---------|-----|
| `sim7080g` | run the daemon in the foreground (what init does) |
| `sim7080g start` | spawn it in the background, when autostart is off |
| `sim7080g status` | link state, address, drops, modem resets |

`CONFIG_SIM7080G_AUTOSTART` (default on) adds the service to `init.rc`; turn it off for
boards without a modem or SIM. init restarts the daemon 5 s after it exits, and a restarted
daemon reuses the running pppd.

The daemon mirrors `sim7080g_task()` from the ESP-IDF firmware:

1. Sends `+++` (a modem left in data mode by a previous run) and probes `AT`. A silent modem
   gets a PWRKEY pulse (`/dev/sim7080g_pwrkey`, GPIO14, 1.2 s).
2. Starts pppd in its own task. The connect script runs on every (re)connect: `ATE0`, `ATH`
   (drops a stale data call), `AT+CNMP=38`, `AT+CMNB=<mode>`, `AT+CGDCONT=1,"IP","<APN>"`,
   waits for `+CGATT: 1`, then `ATD*99#`. pppd retries it on its own until the modem is
   attached; IPCP sets the address, the default route and the operator's DNS servers.
3. Watches `ppp0`. With no address for `CONFIG_SIM7080G_LINK_TIMEOUT` seconds (300), it
   pulses PWRKEY to power-cycle a hung modem. The key toggles power, so a modem that was
   off comes back on the next window.

Settings live under **Application Configuration → VMflow → SIM7080G** (`make menuconfig`):
autostart, APN (`lf.br`), network mode (Cat-M / NB-IoT / both), serial device, link
timeout and PAP credentials. The PWRKEY pin is a board option (`VMFLOW_SIM7080G_PWRKEY_GPIO`).

Notes:

- `NET_TUN` is a BSD-licensed NuttX component, hence `CONFIG_ALLOW_BSD_COMPONENTS=y`.
- pppd only asks the peer for DNS servers when `IPCP_GET_PRI_DNS`/`IPCP_GET_SEC_DNS` are
  defined. `firmware/apps/sim7080g/Make.defs` adds them for the pppd build only, so the
  submodule stays untouched.
- `ppp0` MTU is 1024 (`CONFIG_NET_TUN_PKTSIZE`), matching pppd's 1024-byte buffers.
- pppd echoes the chat dialogue to the console; that output is hardwired in pppd.
- `kill` cannot stop the daemon: without `CONFIG_SIG_DEFAULT` NuttX ignores signals that
  have no handler.
- No CMUX: while PPP is up the modem takes no AT commands, same as the ESP-IDF firmware.

## RGB LED

The board's addressable RGB LED (WS2812) sits on GPIO21. The bringup puts an RMT TX channel
on it and registers NuttX's WS2812 driver as `/dev/leds0`; the `rgbled` command drives it.

```
nsh> rgbled red              # solid color
nsh> rgbled 00ff00 20        # RRGGBB or #RRGGBB, 20 % brightness
nsh> rgbled blink blue 400 3 # 400 ms period, 3 blinks
nsh> rgbled rainbow 5 50     # color wheel for 5 s at 50 %
nsh> rgbled off
```

Colors by name: `off red green blue yellow cyan magenta white orange purple`. The LED holds
the last color written. Applications can write it directly: open `/dev/leds0` and write one
`uint32_t` per LED as `0x00RRGGBB` (the driver reorders it to GRB on the wire).

The pin is a board option (`VMFLOW_RGBLED_GPIO`); the command lives under
**Application Configuration → VMflow → RGB LED (WS2812)**.

## Configuration

```bash
make menuconfig      # edit nuttx/.config
make savedefconfig   # store it back into firmware/boards/vmflow-esp32s3/configs/<CONFIG>/defconfig
```

## Updating NuttX

`nuttx` and `apps` are pinned to the same release tag. To move both to a new release:

```bash
for m in nuttx apps; do
  git -C $m fetch --depth 1 origin tag nuttx-X.Y.Z
  git -C $m checkout nuttx-X.Y.Z
done
make configure build      # rebuild and test on the board
git add nuttx apps && git commit -m "build: bump NuttX to X.Y.Z"
```

Update the toolchain/esptool versions in `docker/Dockerfile` if the new release's
`tools/ci/docker/linux/Dockerfile` changed them.

## License

[MIT](LICENSE) for the VMflow code. NuttX, nuttx-apps and the board code derived from
`esp32s3-devkit` (`firmware/boards/vmflow-esp32s3`) are Apache-2.0.
