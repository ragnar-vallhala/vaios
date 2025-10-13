# Base image
FROM ubuntu:22.04

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install required tools
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    unzip \
    python3 \
    python3-pip \
    qemu-system-arm \
    gcc-arm-none-eabi \
    gdb-multiarch \
    make \
 && rm -rf /var/lib/apt/lists/*

# Set working directory inside container
WORKDIR /project

# Copy entire project into container
COPY . /project

# Ensure run script is executable
RUN chmod +x tools/qemu_no_hal.sh

# Default command: run your existing script
CMD ["tools/qemu_no_hal.sh"]
