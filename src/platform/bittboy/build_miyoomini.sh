#!/bin/sh
# OpenLara for the Miyoo Mini Plus, built on the bittboy target.
# Uses the software rasterizer over SDL 1.2 (no OpenGL, no SwiftShader).
SYSROOT=/root/workspace/mini/arm-buildroot-linux-gnueabihf/sysroot

arm-linux-gnueabihf-g++ \
  -std=c++11 -O3 -g -funwind-tables -ffast-math \
  -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7ve+simd \
  -fno-unroll-loops -fno-exceptions -fno-rtti \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -D__MIYOO__ -DNDEBUG -D_POSIX_THREADS -D_POSIX_READER_WRITER_LOCKS \
  --sysroot=$SYSROOT -I$SYSROOT/usr/include \
  main.cpp crash_handler.cc \
  ../../libs/stb_vorbis/stb_vorbis.c \
  ../../libs/minimp3/minimp3.cpp \
  ../../libs/tinf/tinflate.c \
  -I../../ -I/root/workspace/sdl2-miyoo/mini/inc \
  -o ../../../bin/OpenLara-sw-debug \
  -L$SYSROOT/usr/lib -L/root/workspace/sdl2-miyoo/mini/lib \
  -lm -lpthread -lSDL -lmi_ao -lmi_sys -lmi_common \
  -Wl,--allow-shlib-undefined || exit 1

# Same code, symbols stripped: small to copy over the slow Wi-Fi, and the
# addresses in crash_log.txt still match the -debug build for addr2line.
arm-linux-gnueabihf-strip -o ../../../bin/OpenLara-sw ../../../bin/OpenLara-sw-debug
