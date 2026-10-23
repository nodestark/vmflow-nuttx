# NuttX build environment for VMflow (ESP32-S3).
#
# Toolchain and esptool versions follow apache/nuttx tools/ci/docker/linux/Dockerfile
# for the release pinned in the `nuttx` submodule.

FROM ubuntu:24.04

ARG XTENSA_TOOLCHAIN_URL=https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20241119/xtensa-esp-elf-14.2.0_20241119-x86_64-linux-gnu.tar.xz
ARG ESPTOOL_VERSION=5.2.0
ARG NUTTX_TOOLS_URL=https://github.com/patacongo/tools/archive/9ad3e1ee75c7d39bfee8019ce44f4fd035d89584.tar.gz

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      bison \
      build-essential \
      ca-certificates \
      curl \
      file \
      flex \
      gettext \
      genromfs \
      git \
      gperf \
      libncurses-dev \
      pkg-config \
      python3 \
      python3-pip \
      python3-venv \
      texinfo \
      xxd \
      xz-utils \
 && rm -rf /var/lib/apt/lists/*

# Extra CA certificates (TLS-inspecting proxies). Supplied by `make image` through the
# `extra-ca` build context; defaults to the empty docker/ca directory.
COPY --from=extra-ca . /usr/local/share/ca-certificates/
RUN update-ca-certificates
ENV PIP_CERT=/etc/ssl/certs/ca-certificates.crt \
    REQUESTS_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt

RUN mkdir -p /opt/xtensa-esp-elf \
 && curl -fsSL --retry 5 --retry-all-errors -o /tmp/xtensa.tar.xz "${XTENSA_TOOLCHAIN_URL}" \
 && tar -C /opt/xtensa-esp-elf --strip-components 1 -xJf /tmp/xtensa.tar.xz \
 && rm /tmp/xtensa.tar.xz

# kconfig-tweak / kconfig-mconf from the NuttX tools repo (not packaged in Ubuntu 24.04).
RUN mkdir -p /tmp/nuttx-tools \
 && curl -fsSL --retry 5 --retry-all-errors "${NUTTX_TOOLS_URL}" \
      | tar -C /tmp/nuttx-tools --strip-components 1 -xz \
 && cd /tmp/nuttx-tools/kconfig-frontends \
 && ./configure --enable-mconf --disable-gconf --disable-qconf --disable-nconf \
      --enable-static --disable-shared --prefix=/opt/kconfig-frontends \
 && make -j"$(nproc)" install \
 && rm -rf /tmp/nuttx-tools

RUN python3 -m venv /opt/venv \
 && /opt/venv/bin/pip install --no-cache-dir \
      "esptool==${ESPTOOL_VERSION}" \
      kconfiglib \
      pyserial

ENV PATH="/opt/xtensa-esp-elf/bin:/opt/kconfig-frontends/bin:/opt/venv/bin:${PATH}"

# Build trees are bind-mounted from the host; git refuses to touch them otherwise.
RUN git config --system --add safe.directory '*'

WORKDIR /work
