#!/bin/bash

set -ex

rm -rf build-* *.elf *.com *.dbg *.so

# Build with cosmoc++
mkdir -p build-cosmo
cd build-cosmo
CC=cosmocc CXX=cosmoc++ cmake .. -DBUILD_EXE=ON
make -j
cd ..

# Build with g++
mkdir -p build-gcc
cd build-gcc
CC=cc CXX=c++ cmake ..
make -j
cd ..

cp build-cosmo/cosmo.com .
cp build-gcc/libnative.so .
./cosmo.com