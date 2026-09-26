# Testing on LG's webOS TV 4.0 and 6.0 emulators

webOS 4's compositor differs from webOS 25's in ways that crash or hide
Firefox, and no webOS 4 TV is at hand. LG's webOS TV 4.0 emulator runs that
compositor, so Firefox is tested there. It is a 32-bit x86 system, so it needs
its own 32-bit x86 build of Firefox; nothing here goes into the TV packages.
The emulator has no Mali GPU, so GPU rendering is not tested there.

## The emulator

    build/emu/setup-emulator.sh      # into ~/webos4-emulator (EMU_DIR to change)

It downloads `Emulator_tv_linux_v4.0.0.zip` from the Internet Archive
(archive.org/details/lg-webos-tv-emulator, a copy of LG's SDK emulator, 1.26
GB; `ZIP=path` uses a local copy), converts the VirtualBox disk for QEMU and
adds root access for testing. It needs QEMU (`brew install qemu`) and the
ffbuild container running. Takes under a minute after the download.

`build/emu/emu.sh start` then boots it headless in QEMU with the VM's own
hardware (x86 emulation, so it also runs on Apple Silicon); the welcome screen
is up after about 90 seconds. `emu.sh stop` powers it off.

Root access: LG's dropbear refuses root (`-w`) and the SDK key is not
accepted. The system lives on an encrypted partition that LG's boot script
unlocks; that script, and everything in `/etc/init` on the plain root
partition, must stay unchanged or the unlock fails. So the setup script adds
two files with `debugfs` and changes nothing else: a new key's public half as
`/emu-test/authorized_keys` on the root partition, and a Developer Mode start
script on the unencrypted media partition (`hda4`),
`/cryptofs/apps/usr/palm/services/com.palmdts.devmode.service/start-devmode.sh`,
which LG's `devmode` job runs as root at boot. It installs the key for root
and starts a second dropbear on port 2222 that takes keys only. QEMU forwards
it to 127.0.0.1:6623; the other forwards and QEMU's monitor also listen on
127.0.0.1 only.

## Build and run

    podman exec ffbuild bash /src/build/emu/build-i686.sh      # Firefox, i686
    podman exec ffbuild bash -c 'CROSS=i686 OUT=/work/emu-app/firefox-runtime bash /src/build/build-adapter.sh'
    podman exec ffbuild bash /src/build/emu/assemble-emu.sh    # -> /work/emu-app.tar.gz
    build/emu/emu.sh deploy                                    # copy it in
    build/emu/emu.sh firefox                                   # (re)start it

Rerun `assemble-emu.sh` after rebuilding the adapter: `deploy` copies the
packed build. It is unpacked as the TV app's path,
`/media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv`, which
the GTK caches assume, and started through the same launcher as on a TV. The
build carries Debian 12's glibc, since the emulator's is 2.24; its executables
load it through `/tmp/ld32.so` (`set-interp.py`, `run-firefox.sh`).

`emu.sh` drives the rest: `shot` (screenshot), `click X Y`, `key`, `ssh`,
`put`, `get`.

## webOS 6.0

    WEBOS=6 build/emu/setup-emulator.sh    # into ~/webos6-emulator
    WEBOS=6 build/emu/emu.sh start         # and every other emu.sh command

The same steps with `Emulator_tv_linux_v6.0.0.zip` (1.34 GB), plus three
things 6.0 needs under QEMU:

- Its compositor needs OpenGL on a DRM display. VirtualBox gives it VMware
  SVGA 3D; QEMU on this Mac has no host 3D. So `emu.sh` gives 6.0 a
  `virtio-vga` display (1920x1080), and the setup script puts a software
  OpenGL driver on the media partition as `/developer/mesa-sw`: Mesa's
  `kms_swrast` from Debian 10 (glibc 2.28, like the image; about 21 MB from
  archive.debian.org, cached in `~/webos6-emulator/mesa-deb10`) with the
  libraries the image lacks. The Developer Mode script points the compositor
  at it through the compositor's optional environment file,
  `/var/systemd/system/env/surface-manager.env`, and restarts it once.
- Its first boot copies the media partition's contents out of the encrypted
  system and does not run the Developer Mode script; later boots do. The
  setup script does that first boot itself.
- LG's web apps (the home screen) need GPU buffer sharing, which the software
  driver lacks, so the screen stays black until Firefox runs. Firefox draws
  into shared memory and shows.

To start Firefox the way a TV does, install it through webOS's installer
and launch it through the app manager:

    podman exec ffbuild bash /src/build/emu/assemble-emu.sh   # also dist/*_i586.ipk
    WEBOS=6 build/emu/emu.sh install
    WEBOS=6 build/emu/emu.sh launch

The installer leaves the app folder root's, as on a TV. 6.0's app manager
starts native apps as root through `jailer`, which the image lacks, so
`install` adds `build/emu/jailer-standin.c` as `/usr/bin/jailer` and also in
place of the app's launcher (renamed `geckotv.real`), so the app runs as its
own user (6795:5000, plus the compositor group) like on a TV. The app manager
kills an app whose window is not up within 10 seconds; a cold start under
TCG can miss that, a second launch usually does not.

The emulator's compositor is not quite the TV's: it offers `wl_subcompositor`
and `wl_data_device_manager`, which a webOS 6 TV (webOS 6.5, 2021 NANO796PC)
does not. `WEBOS_XDG_AS_WEBOS4=1` in the app's `env` file hides them, so the
adapter takes the path it takes on those TVs.
