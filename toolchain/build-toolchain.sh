#!/usr/bin/env bash
# Builds the KallistiOS sh-elf toolchain (binutils + gcc + newlib) and KallistiOS itself.
# Mirrors the configure flags used by KOS's kos-chain "stable" profile, but fetches
# sources from the Ubuntu archive (which is reachable from most CI runners and
# mirrors exactly the same upstream tarballs).
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

PREFIX=${PREFIX:-/opt/toolchains/dc}
SH=${PREFIX}/sh-elf
KOS=${KOS_BASE:-${PREFIX}/kos}
WORK=${WORK:-$HOME/dc-toolchain-build}
JOBS=${JOBS:-$(nproc)}
UBU=http://archive.ubuntu.com/ubuntu/pool

BINUTILS_VER=2.45
GCC_VER=15.2.0
NEWLIB_VER=4.6.0.20260123
TARGET=sh-elf
CPU_ARGS="--with-multilib-list=m4-single,m4-single-only --with-endian=little --with-cpu=m4-single"

mkdir -p "$WORK" "$SH"
cd "$WORK"

fetch() { [ -f "$2" ] || curl -fL --retry 3 -o "$2" "$1"; }
fetch $UBU/main/b/binutils/binutils_${BINUTILS_VER}.orig.tar.xz binutils.tar.xz
fetch $UBU/main/g/gcc-15/gcc-15_${GCC_VER}.orig.tar.gz gcc-pkg.tar.gz
fetch $UBU/universe/n/newlib/newlib_${NEWLIB_VER}.orig.tar.xz newlib.tar.xz

if [ ! -d "$KOS" ]; then
  git clone --depth 1 https://github.com/KallistiOS/KallistiOS "$KOS"
fi
PATCHES=$KOS/utils/kos-chain/patches

[ -d binutils-$BINUTILS_VER ] || tar xf binutils.tar.xz
if [ ! -d gcc-$GCC_VER ]; then
  # The Ubuntu "orig" tarball wraps the upstream gcc tarball.
  mkdir -p gccpkg && tar xf gcc-pkg.tar.gz -C gccpkg
  inner=$(find gccpkg -name "gcc-${GCC_VER}*.tar.*" | head -1)
  if [ -n "$inner" ]; then tar xf "$inner"; else mv gccpkg/gcc-${GCC_VER}* gcc-$GCC_VER; fi
  cp $PATCHES/gcc/gthr-kos.h gcc-$GCC_VER/libgcc/gthr-kos.h
  cp $PATCHES/gcc/fake-kos.c gcc-$GCC_VER/libgcc/config/fake-kos.c
  patch -N -d gcc-$GCC_VER -p1 -i $PATCHES/targets/gcc-${GCC_VER}-kos.diff
fi
if [ ! -d newlib-$NEWLIB_VER ]; then
  tar xf newlib.tar.xz
  mkdir -p newlib-$NEWLIB_VER/newlib/libc/machine/sh/sys
  cp $KOS/include/sys/lock.h newlib-$NEWLIB_VER/newlib/libc/machine/sh/sys/lock.h
  cp $KOS/include/sys/lock.h newlib-$NEWLIB_VER/newlib/libc/include/sys/lock.h
  for p in $PATCHES/targets/newlib-${NEWLIB_VER}*.diff; do [ -f "$p" ] && patch -N -d newlib-$NEWLIB_VER -p1 -i "$p"; done
fi
export PATH=$SH/bin:$PATH

if [ ! -x $SH/bin/sh-elf-as ]; then
  mkdir -p b-binutils && cd b-binutils
  ../binutils-$BINUTILS_VER/configure --target=$TARGET --prefix=$SH --disable-werror CFLAGS="-O2 -std=gnu17"
  make -j$JOBS && make install-strip; cd ..
fi
if [ ! -x $SH/bin/sh-elf-gcc ]; then
  mkdir -p b-gcc1 && cd b-gcc1
  ../gcc-$GCC_VER/configure --target=$TARGET --prefix=$SH --with-gnu-as --with-gnu-ld --without-headers \
    --with-newlib --enable-languages=c --disable-libssp --enable-checking=release --disable-nls $CPU_ARGS MAKEINFO=missing
  make -j$JOBS all-gcc all-target-libgcc && make install-strip-gcc install-target-libgcc; cd ..
fi
if [ ! -f $SH/$TARGET/lib/libc.a ]; then
  mkdir -p b-newlib && cd b-newlib
  ../newlib-$NEWLIB_VER/configure --disable-newlib-supplied-syscalls --target=$TARGET --prefix=$SH $CPU_ARGS \
    --enable-newlib-io-c99-formats --enable-target-optspace
  make -j$JOBS && make install; cd ..
  inc=$SH/$TARGET/include; mkdir -p $inc/sys $inc/machine
  cp $KOS/include/pthread.h $inc; cp $KOS/include/sys/_pthreadtypes.h $inc/sys
  cp $KOS/include/sys/dirent.h $inc/sys; cp $KOS/include/machine/time.h $inc/machine
  ln -nsf $KOS/include/kos $inc
fi
if [ ! -x $SH/bin/sh-elf-g++ ]; then
  mkdir -p b-gcc2 && cd b-gcc2
  ../gcc-$GCC_VER/configure --target=$TARGET --prefix=$SH --with-gnu-as --with-gnu-ld --with-newlib \
    --disable-libssp --enable-threads=kos --enable-languages=c,c++ --enable-checking=release --disable-nls $CPU_ARGS MAKEINFO=missing
  make -j$JOBS && make install-strip; cd ..
  for m in "" -m4-single-only; do
    lib=$($SH/bin/sh-elf-gcc $m -print-file-name=libgcc.a); $SH/bin/sh-elf-gcc-ar d "$lib" fake-kos.o || true
  done
fi

# Build KallistiOS (with UTDC patches)
cd "$KOS"
for p in "$SCRIPT_DIR"/patches/kos-*.patch; do
  [ -f "$p" ] || continue
  if patch -p1 --dry-run -N -s -i "$p" >/dev/null 2>&1; then patch -p1 -N -i "$p"; fi
done
[ -f environ.sh ] || cp doc/environ.sh.sample environ.sh
sed -i "s#^export KOS_BASE=.*#export KOS_BASE=\"$KOS\"#" environ.sh || true
set +u; source ./environ.sh; set -u
make -j$JOBS
make -C utils/scramble utils/makeip 2>/dev/null || true
echo "Toolchain ready. source $KOS/environ.sh"
