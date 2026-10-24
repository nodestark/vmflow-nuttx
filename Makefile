# VMflow NuttX firmware — build, flash and monitor inside Docker.
#
#   make image       build the Docker toolchain image
#   make configure   apply $(BOARD_DIR)/configs/$(CONFIG)
#   make build       compile nuttx/nuttx.bin
#   make flash       write the image to $(PORT)
#   make monitor     open the NSH console on $(PORT)
#   make menuconfig  edit the config; `make savedefconfig` stores it back

IMAGE  ?= vmflow-nuttx-builder
BOARD_DIR := firmware/boards/vmflow-esp32s3
CONFIG ?= vmflow
PORT   ?= /dev/ttyACM0
BAUD   ?= 115200
JOBS   ?= $(shell nproc)

# Host CA store, forwarded into the image so builds work behind TLS-inspecting proxies.
EXTRA_CA_DIR ?= $(or $(wildcard /usr/local/share/ca-certificates),docker/ca)

TTY        := $(shell [ -t 0 ] && echo -it)
DOCKER_RUN := docker run --rm $(TTY) \
	--user $(shell id -u):$(shell id -g) -e HOME=/tmp \
	-v $(CURDIR):/work -w /work/nuttx
DEVICE     = --device $(PORT) --group-add $(shell stat -c %g $(PORT) 2>/dev/null || echo dialout)

.PHONY: all image configure build flash monitor menuconfig savedefconfig distclean shell

all: build

image:
	docker build --build-context extra-ca=$(EXTRA_CA_DIR) -t $(IMAGE) docker

apps/external:
	ln -sfn ../firmware/apps apps/external

configure: apps/external
	@# A stale config whose Make.defs link points nowhere breaks `-E` (distclean)
	@test -e nuttx/Make.defs || rm -f nuttx/.config nuttx/Make.defs
	$(DOCKER_RUN) $(IMAGE) ./tools/configure.sh -E -l ../$(BOARD_DIR)/configs/$(CONFIG)

nuttx/.config:
	$(MAKE) configure

build: apps/external nuttx/.config
	$(DOCKER_RUN) $(IMAGE) make -j$(JOBS)

flash:
	$(DOCKER_RUN) $(DEVICE) $(IMAGE) make flash ESPTOOL_PORT=$(PORT)

# On the USB-Serial/JTAG, DTR and RTS drive the chip's BOOT and RESET lines, and RTS high with
# DTR low resets it. The port opens with both high; dropping DTR before RTS (miniterm's order
# with --dtr 0 --rts 0) passes through that state, so leave both as the open set them.

monitor:
	$(DOCKER_RUN) $(DEVICE) $(IMAGE) python3 -m serial.tools.miniterm --raw $(PORT) $(BAUD)

menuconfig: nuttx/.config
	$(DOCKER_RUN) $(IMAGE) make menuconfig

savedefconfig: nuttx/.config
	$(DOCKER_RUN) $(IMAGE) make savedefconfig
	cp nuttx/defconfig $(BOARD_DIR)/configs/$(CONFIG)/defconfig

distclean:
	$(DOCKER_RUN) $(IMAGE) make distclean

shell:
	$(DOCKER_RUN) $(IMAGE) bash
