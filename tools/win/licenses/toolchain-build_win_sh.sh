#!/bin/bash
# canadian cross: build=linux, host=x86_64-w64-mingw32, target=sh-elf  ->  /opt/sh-elf-win
set -e
S=/tmp/claude-0/xsrc; W=$S/work; P=/opt/sh-elf-win; D=$S/deps
H=x86_64-w64-mingw32
mkdir -p $W $D; cd $W
export CFLAGS="-O2" CXXFLAGS="-O2" LDFLAGS="-static -static-libgcc -static-libstdc++"
log(){ echo "=== $(date +%T) $*"; }
log extract
[ -d binutils-2.42 ] || tar xf $S/binutils_2.42.orig.tar.xz
[ -d gmp-6.3.0+dfsg ] || tar xf $S/gmp_6.3.0+dfsg.orig.tar.xz
[ -d mpfr-4.2.1 ] || tar xf $S/mpfr4_4.2.1.orig.tar.xz
[ -d mpc-1.3.1 ] || tar xzf $S/mpclib3_1.3.1.orig.tar.gz
if [ ! -d gcc-13.3.0 ]; then tar xzf $S/gcc-13_13.3.0.orig.tar.gz gcc-13-13.3.0/gcc-13.3.0.tar.xz; tar xf gcc-13-13.3.0/gcc-13.3.0.tar.xz; fi
log binutils
if [ ! -x $P/bin/sh-elf-as.exe ]; then
mkdir -p b-binutils && cd b-binutils
[ -f Makefile ] || ../binutils-2.42/configure --host=$H --target=sh-elf --prefix=$P --disable-nls --disable-werror \
    --disable-gdb --disable-gdbserver --disable-sim --disable-gprof --without-zstd --disable-plugins > cfg.log 2>&1
make -j2 > make.log 2>&1; make install > inst.log 2>&1; cd ..
fi
log gmp
mkdir -p b-gmp && cd b-gmp; [ -f Makefile ] || ../gmp-6.3.0+dfsg/configure --host=$H --prefix=$D --disable-shared --enable-static > cfg.log 2>&1
make -j2 > make.log 2>&1; make install > inst.log 2>&1; cd ..
log mpfr
mkdir -p b-mpfr && cd b-mpfr; [ -f Makefile ] || ../mpfr-4.2.1/configure --host=$H --prefix=$D --disable-shared --enable-static --with-gmp=$D > cfg.log 2>&1
make -j2 > make.log 2>&1; make install > inst.log 2>&1; cd ..
log mpc
mkdir -p b-mpc && cd b-mpc; [ -f Makefile ] || ../mpc-1.3.1/configure --host=$H --prefix=$D --disable-shared --enable-static --with-gmp=$D --with-mpfr=$D > cfg.log 2>&1
make -j2 > make.log 2>&1; make install > inst.log 2>&1; cd ..
log gcc
export PATH=/opt/sh-elf/bin:$PATH
mkdir -p b-gcc && cd b-gcc
[ -f Makefile ] || ../gcc-13.3.0/configure --build=x86_64-linux-gnu --host=$H --target=sh-elf --prefix=$P \
    --with-gmp=$D --with-mpfr=$D --with-mpc=$D --enable-languages=c --without-headers --with-newlib \
    --disable-libssp --disable-nls --disable-threads --disable-shared --disable-libquadmath --disable-libgomp \
    --with-cpu=m2 --with-multilib-list=m2 --disable-lto --disable-plugin > cfg.log 2>&1
make -j2 all-gcc > make.log 2>&1; make install-gcc > inst.log 2>&1; cd ..
log libgcc-copy
cp -r /opt/sh-elf/lib/gcc/sh-elf/13.3.0/. $P/lib/gcc/sh-elf/13.3.0/
log done
