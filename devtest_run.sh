#!/bin/bash

cd build

export WINEARCH=win32
export WINEPREFIX="$HOME/.wine32"

WINEPREFIX="$HOME/.wine32" wine visdriver.exe generate-verification-data   --runtime-dir ./   --vis-avs-dat ./vis_avs.dat  --vis-dll ./vis_avs.dll  --out-dll ./out_wave.dll --preset ./tests/data/phase1/color_mod.avs --wav ./tests/data/test.wav --out-dir ./tests/out/ --avi-out test.avi
