# syntax=docker/dockerfile:1
# =============================================================================
# vaios container build. Multi-stage so the same toolchain serves both an
# interactive dev shell and a baked run-it-in-QEMU image.
#
#   # interactive dev shell (bind-mount your working tree):
#   docker build --target dev -t vaios:dev .
#   docker run --rm -it -v "$PWD:/project" vaios:dev
#
#   # one-shot: build an example + boot it under QEMU (default target):
#   docker build -t vaios:qemu .
#   docker run --rm vaios:qemu                       # FIFO_TEST
#   docker run --rm -e VAIOS_EXAMPLE=PERF_DEMO vaios:qemu
#   docker build --build-arg VAIOS_EXAMPLE=IPC_TEST -t vaios:qemu .
# =============================================================================

# ---------------------------------------------------------------------------
# base — ARM bare-metal toolchain, QEMU and debugger. Shared by every target.
# ---------------------------------------------------------------------------
# 24.04 ships gcc-arm-none-eabi 13.2.rel1 + newlib; the 22.04 toolchain (10.3)
# miscompiles the kernel at -O2 and the no-HAL examples HardFault under QEMU.
FROM ubuntu:24.04 AS base

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install required tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    wget \
    unzip \
    python3 \
    python3-pip \
    qemu-system-arm \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    gdb-multiarch \
    make \
 && rm -rf /var/lib/apt/lists/*

# Set working directory inside container
WORKDIR /project

# ---------------------------------------------------------------------------
# dev — interactive development environment. No source is copied in; mount the
# repo at /project so edits on the host build inside the container. Gives you
# cmake / arm-none-eabi-gcc / qemu-system-arm / gdb-multiarch on a clean box.
# ---------------------------------------------------------------------------
FROM base AS dev

# Default example used by tools/qemu_no_hal.sh when you just run the script.
ENV VAIOS_EXAMPLE=FIFO_TEST

CMD ["bash"]

# ---------------------------------------------------------------------------
# qemu — default target. Bake the source in, build a no-HAL example and boot
# it under QEMU. Choose the example at build time (--build-arg VAIOS_EXAMPLE)
# or at run time (-e VAIOS_EXAMPLE).
# ---------------------------------------------------------------------------
FROM base AS qemu

ARG VAIOS_EXAMPLE=FIFO_TEST
ENV VAIOS_EXAMPLE=${VAIOS_EXAMPLE}

# Copy entire project into container
COPY . /project

# Ensure run script is executable
RUN chmod +x tools/qemu_no_hal.sh

# Default command: build the selected example and run it under QEMU
CMD ["tools/qemu_no_hal.sh"]
