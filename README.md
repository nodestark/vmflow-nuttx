# VMflow NuttX

[![build](https://img.shields.io/github/actions/workflow/status/nodestark/vmflow-nuttx/build.yml?branch=main&label=build)](https://github.com/nodestark/vmflow-nuttx/actions/workflows/build.yml)
[![NuttX](https://img.shields.io/badge/NuttX-13.1.0-blue)](https://github.com/apache/nuttx/releases/tag/nuttx-13.1.0)
[![chip](https://img.shields.io/badge/chip-ESP32--S3-red)](https://www.espressif.com/en/products/socs/esp32-s3)
[![modem](https://img.shields.io/badge/modem-SIM7080G%20LTE--M%2FNB--IoT-orange)](#sim7080g-lte-m--nb-iot)
[![docker](https://img.shields.io/badge/toolchain-Docker-2496ED?logo=docker&logoColor=white)](docker/Dockerfile)
[![license](https://img.shields.io/github/license/nodestark/vmflow-nuttx)](LICENSE)
[![last commit](https://img.shields.io/github/last-commit/nodestark/vmflow-nuttx)](https://github.com/nodestark/vmflow-nuttx/commits/main)

NuttX port of the [VMflow](https://github.com/nodestark/mdb-esp32-cashless) MDB cashless
firmware for the ESP32-S3.

> **Status** — bring-up. NuttX boots with an NSH console on the chip's native USB port
> (USB-Serial/JTAG). The SIM7080G comes up as a regular network interface (`ppp0`) through
> pppd, so sockets, DNS and HTTP go over LTE-M/NB-IoT. The `vmflow` application keeps an MQTT
> session with the VMflow broker (status, last will, subscription); MDB, Wi-Fi, the signed RPC
> handler, BLE provisioning and OTA from the ESP-IDF firmware are not ported yet. The WS2812
> status LED works through the `rgbled` command, and `dex` prints the machine's EVA-DTS audit.

Related projects:

- [mdb-esp32-cashless](https://github.com/nodestark/mdb-esp32-cashless): the ESP-IDF
  firmware this port follows, with the hardware (KiCad), backend and Android app.
- [vmflow-cli](https://github.com/nodestark/vmflow-cli): command line for the VMflow fleet
  (auth, device RPC over MQTT, sales, telemetry).

## Layout

```
.
├── nuttx/                    apache/nuttx      (submodule, nuttx-13.1.0)
├── apps/                     apache/nuttx-apps (submodule, nuttx-13.1.0)
├── firmware/
│   ├── apps/                 VMflow applications, linked as apps/external
│   │   ├── vmflow/           cashless application (`vmflow` NSH command), MQTT client
│   │   ├── sim7080g/         modem daemon: power, pppd, link supervision
│   │   ├── rgbled/           `rgbled` command for the WS2812 status LED
│   │   └── dex/              `dex` command: EVA-DTS audit over DDCMP or DEX/UCS
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

If the console stays silent after `make flash`, the chip did not leave download mode on
the final reset; replug the board or press RESET.

Flashing and the console share the chip's native USB port (GPIO19/20), which shows up as
`303a:1001` Espressif USB JTAG/serial in both download mode and while NuttX runs.

On the USB-Serial/JTAG, DTR and RTS drive the chip's BOOT and RESET lines: RTS high with
DTR low resets the chip, DTR high with RTS low pulls BOOT low. Linux raises both when the
port opens, which is harmless, so `make monitor` leaves them there. A terminal that then
drops DTR before RTS (miniterm's `--dtr 0 --rts 0`) passes through the reset state and
reboots the board on every open.

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
- pppd drops the link after sending `AHDLC_TX_OFFLINE` frames with nothing received. Upstream
  uses 5, which a few back-to-back TCP segments hit within one LTE-M round trip;
  `CONFIG_SIM7080G_PPP_TX_OFFLINE` (30) overrides it through `ppp_tuning.h`, force-included
  into the pppd build.
- TCP write buffers are on (`NET_TCP_WRITE_BUFFERS`, 64 IOBs). Without them every segment
  waits for its ACK and a non-blocking `send()` returns `EAGAIN`, which MQTT-C spins on.

## MQTT

`vmflow mqtt` is the MQTT client ([MQTT-C](https://github.com/LiamBindle/MQTT-C), plain TCP),
set up like the esp-mqtt client of the ESP-IDF firmware:

| | |
|-|-|
| broker | `mqtt.vmflow.xyz:1883`, anonymous, keepalive 120 s, clean session |
| client id | `vmflow-<subdomain>` |
| last will | `offline`, QoS 1, retained, on `domain.vmflow.xyz/<subdomain>/status` |
| on connect | publishes `online` (QoS 1, retained) there and subscribes `<subdomain>.vmflow.xyz/#` |
| reconnect | every 15 s; DNS or connect fail while `ppp0` is down, so it just waits for the link |

The device id (`<subdomain>`) is a build option for now: the ESP-IDF firmware gets it over BLE
and keeps it in NVS. With it empty (the default) the client does not start.

```bash
make menuconfig    # Application Configuration → VMflow → Device subdomain
make build flash
```

```
nsh> vmflow mqtt status
client  : pid 7, device 123
broker  : mqtt.vmflow.xyz:1883, keepalive 120 s
state   : connected for 56 s
sessions: 1, failed attempts: 1
messages: rx 5, tx 6, late acks 0
nsh> vmflow mqtt pub domain.vmflow.xyz/123/test hello 1
```

| Command | Use |
|---------|-----|
| `vmflow mqtt` | run the client in the foreground (what init does) |
| `vmflow mqtt start` | spawn it in the background, when autostart is off |
| `vmflow mqtt status` | session state and counters |
| `vmflow mqtt pub <topic> <msg> [qos] [retain]` | publish through the running session |

With a subdomain set, `CONFIG_VMFLOW_MQTT_AUTOSTART` (default on) adds the client to
`init.rc` next to the modem service. Incoming messages are only logged to syslog until the
RPC handler (`<cmd>:<params>:<ts>:<hmac>`) is ported. The broker ACL only lets anonymous
clients use `+/rpc`, `+/credit` and `domain.vmflow.xyz/#`.

LTE-M has stalls of several seconds (and occasionally tens). MQTT-C resends any request still
unacknowledged after 30 s, so both acks may come back; the second one matches nothing and MQTT-C
reports `MQTT_ERROR_ACK_OF_UNKNOWN`, which would end the session. The client drops that error and
counts it as `late acks`.

Settings live under **Application Configuration → VMflow** (`make menuconfig`): broker host and
port, keepalive, reconnect interval and buffer sizes. The send buffer (6 KB) holds every message
until it is acknowledged; DEX audit dumps reach ~4 KB.

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

## DEX (EVA-DTS audit)

`dex` reads the vending machine's audit through its DEX port, a port of `eva-dts.c` from the
ESP-IDF firmware. The port is UART2 on the same pins (GPIO9 TX, GPIO8 RX), `/dev/ttyS2`.

```
nsh> dex                     # DDCMP at 2400 baud, then DEX/UCS at 9600 if DDCMP got nothing
nsh> dex ddcmp               # only DDCMP
nsh> dex ucs                 # only DEX/UCS
nsh> dex > /tmp/audit.txt    # keep it in a file
nsh> cat /tmp/audit.txt
DXS*...
```

Only the audit goes to stdout; status lines (`dex: <n> bytes over DEX/UCS`,
`dex: no answer from the machine`) go to stderr and stay on the console when stdout is
redirected. `/tmp` is a tmpfs in RAM, cleared on reboot.

One read at a time: a `dex` started while another is running exits with
`dex: busy, another read is in progress`. The lock is a robust mutex, so a `dex` that dies mid-read
does not leave it held.

Differences from the ESP-IDF version:

- No MQTT publish; the output is plain text to use as needed.
- DEX/UCS only runs when DDCMP returned nothing; the ESP-IDF firmware always runs both.
- A silent line ends the read (the ESP-IDF loop kept reusing the last byte after a timeout), and a
  DDCMP message longer than the 1 KB receive buffer is rejected instead of overflowing it.

Settings live under **Application Configuration → VMflow → EVA-DTS audit reader**: serial device
and audit buffer size (8 KB).

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
