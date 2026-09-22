#!/bin/bash
# Build a minimal 64-bit FFmpeg (H.264, AAC, MP3 decoders only) inside the
# ffbuild container and drop libavcodec/libavutil/libswresample into
# app/firefox-runtime. Firefox dlopens libavcodec.so.60 at runtime and gains
# H.264 + AAC, which its bundled libmozavcodec does not carry. ~2.7 MB.
# Usage: podman exec ffbuild bash /src/build/ffmpeg-mini.sh
set -euo pipefail
R=/src/app/firefox-runtime
BR=/media/developer/apps/usr/palm/applications/org.webosbrew.bridge-64to32/lib
V=6.1.2
apt-get install -y --no-install-recommends nasm yasm pkg-config curl xz-utils patchelf >/dev/null 2>&1 || true
cd /tmp
[ -d ffmpeg-$V ] || { curl -fsSL -o ff.tar.xz https://ffmpeg.org/releases/ffmpeg-$V.tar.xz && tar xf ff.tar.xz; }
cd ffmpeg-$V
./configure --prefix=/tmp/ffmini --enable-shared --disable-static --disable-programs --disable-doc \
  --disable-everything --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-postproc \
  --enable-decoder=h264,aac,aac_latm,mp3,mp3float --enable-parser=h264,aac,aac_latm,mpegaudio \
  --enable-bsf=h264_mp4toannexb,aac_adtstoasc --disable-network --disable-debug --disable-x86asm --enable-pic >/tmp/ffconf.log
make -j"$(nproc)" >/tmp/ffmake.log 2>&1 && make install >/tmp/ffinst.log 2>&1
for f in libavcodec.so.60 libavutil.so.58 libswresample.so.4; do
    cp -L /tmp/ffmini/lib/$f "$R/$f"; patchelf --set-rpath "\$ORIGIN:$BR" "$R/$f"
done
ls -l "$R"/libavcodec.so.60 "$R"/libavutil.so.58 "$R"/libswresample.so.4
echo FFMPEG_MINI_DONE
