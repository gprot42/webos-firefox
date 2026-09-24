# webOS toolchain: any buildroot-nc4 SDK (same compiler name and layout).
# The launcher shipped with the nc4 build is built with the nc4 SDK from
# build/nc4/build-sdk.sh, inside the ffbuild container:
#   podman exec -u builder ffbuild make -C /src -B TC=/work/nc4/out/host app/geckotv
# The default below is an older macOS-hosted SDK (GCC 12.2, glibc 2.12.2)
# for building on the Mac; its output needs the same glibc.
TC ?= /Users/aicoder/toolchains/arm-webos-linux-gnueabi_sdk-buildroot
CC := $(TC)/bin/arm-webos-linux-gnueabi-gcc
SYSROOT := $(TC)/arm-webos-linux-gnueabi/sysroot

CFLAGS := --sysroot=$(SYSROOT) -O2 -Wall -Wextra -std=c11
LDFLAGS := --sysroot=$(SYSROOT) -lwayland-webos-client -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lm -ldl

.PHONY: all package clean icons

all: app/smoke app/geckotv

app/smoke: src/smoke.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	chmod 755 $@

app/geckotv: src/geckotv.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod 755 $@

icons:
	python3 scripts/make-icons.py

package: all
	python3 scripts/pack-ipk.py

clean:
	rm -f app/smoke app/geckotv dist/*.ipk
