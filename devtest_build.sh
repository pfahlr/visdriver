#!/bin/bash
rm -rf build

cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build
make -C build -j$(nproc) VERBOSE=1

cp for_codex/dll/* build/
cp -R for_codex/tests build/



