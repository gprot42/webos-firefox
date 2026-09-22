# Linux host for the Firefox cross compile. The Mac toolchain and the staged
# sysroot are mounted in, not copied. build/build-firefox.sh is what to run
# once the volume has 40 GB free. This image is not used until then.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git python3 python3-pip python3-venv \
        make patch unzip xz-utils pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
