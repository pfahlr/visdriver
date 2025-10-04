[![Build on Linux](https://github.com/hartwork/visdriver/actions/workflows/linux-mingw.yml/badge.svg)](https://github.com/hartwork/visdriver/actions/workflows/linux-mingw.yml)
[![Build on Windows](https://github.com/hartwork/visdriver/actions/workflows/windows-msvc.yml/badge.svg)](https://github.com/hartwork/visdriver/actions/workflows/windows-msvc.yml)
[![Enforce clang-format](https://github.com/hartwork/visdriver/actions/workflows/clang-format.yml/badge.svg)](https://github.com/hartwork/visdriver/actions/workflows/clang-format.yml)


[![screenshots/visdriver_geiss_804x627.png](https://raw.githubusercontent.com/hartwork/visdriver/main/screenshots/visdriver_geiss_804x627.png)](https://github.com/hartwork/visdriver/blob/main/screenshots/visdriver_geiss_804x627.png)

(Re-titled with `wmctrl -r 'Default - Wine desktop' -N 'Geiss @ visdriver (800x600)'`)


# What is visdriver?

**visdriver** is
a Wine/Windows application
that uses **Winamp plug-ins**
to **visualize audio** without actual Winamp/WACUP,
in particular with MinGW on GNU/Linux.
It is written in C99,
uses plain win32api, and
is licensed under the "GPL v3 or later" license.

It needs:
- One input plug-in binary
  (e.g. `in_line.dll`
  [[source]](https://github.com/jaspervdg/lineinWA)
  [[binary]](https://home.hccnet.nl/th.v.d.gronde/dev/lineinWA2/)
  or `in_mad.dll`
  [[source]](https://sourceforge.net/projects/plainamp/files/in_mad/)
  [[binary]](https://www.mars.org/home/rob/proj/mpeg/mad-plugin/#install) for MP3 playback),
- One output plugin binary
  (e.g. `out_wave_gpl.dll`
  [[source]](https://sourceforge.net/projects/plainamp/files/out_wave_gpl/)
  [[binary]](https://sourceforge.net/projects/plainamp/files/Plainamp/0.2.3/)),
- One vis plugin binary
  (e.g. `vis_geis.dll`
  [[source]](https://github.com/geissomatik/geiss)
  [[binary]](https://github.com/geissomatik/geiss/releases)
  or `vis_avs.dll`
  [[source]](https://github.com/grandchild/vis_avs)
  [[binary]](https://github.com/grandchild/vis_avs/actions)),
- A MinGW compiler (or Visual Studio),
- Wine (or Windows),
- CMake >=3.0 (and potentially GNU make or Ninja).


# Download

If would you like to download ready-to-run Windows binaries
built by the CI off the latest code on branch `main`,
there are two options:
- [Binaries built by MinGW/GCC](https://github.com/hartwork/visdriver/actions/workflows/linux-mingw.yml?query=branch%3Amain)
- [Binaries built by Visual Studio](https://github.com/hartwork/visdriver/actions/workflows/windows-msvc.yml?query=branch%3Amain)

Just click the latest workflow run there for either of these, and
its page will list artifacts for download near the bottom.


# How to Compile

## With MinGW/GCC

On a fresh Ubuntu 24.04 environment the MinGW toolchain and Wine runtime are
not available by default. Install the packages that mirror our CI environment
first:

```console
# apt-get update
# apt-get install -y build-essential cmake ccache mingw-w64 wine
```

Once the toolchain is present you can configure and build the MinGW target:

```console
# cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build
# make -C build -j$(nproc) VERBOSE=1
```

## With Visual Studio

```console
# cmake -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build
# cmake --build build
```


# How to Run

Let **visdriver** tell you what it needs:
```console
# WINEDEBUG=-all wine ./build/visdriver.exe --help
Usage: visdriver [OPTIONS] --in PATH/IN.dll --out PATH/OUT.dll --vis PATH/VIS.dll [--] [AUDIO_FILE ..]
   or: visdriver --help
   or: visdriver --version

visdriver uses Winamp plug-ins to visualize audio.

    -h, --help        show this help message and exit
    -V, --version     show the version and exit

Plug-in related arguments:
    -I, --in=<str>    input plug-in to use
    -O, --out=<str>   output plug-in to use
    -W, --vis=<str>   vis plug-in to use

Software libre licensed under GPL v3 or later.
Brought to you by Sebastian Pipping <sebastian@pipping.org>.

Please report bugs at https://github.com/hartwork/visdriver -- thank you!
```

If you end up with errors about missing DLLs, copying these files in place
should help.  E.g. for MinGW DLLs on Ubuntu 24.04 it would be:

```console
# cp -v \
    /usr/i686-w64-mingw32/lib/libwinpthread-1.dll \
    /usr/lib/gcc/i686-w64-mingw32/*-posix/libgcc_s_dw2-1.dll \
    /usr/lib/gcc/i686-w64-mingw32/*-posix/libstdc++-6.dll \
    .
```

The locations of these files vary among GNU/Linux distros.


## `visdriver.exe generate-verification-data` operation

visdriver.ex has beem extended to provide the `generate-verification-data` operation. This has been implemented specifically to support generation of test data
to be used in the development of new versions/ports of AVS. 

### CLI-options
```
visdriver.exe generate-verification-data --help
Options:
  --vis-dll <path>      Path to vis DLL (required)
  --runtime-dir <dir>   Runtime directory (default: directory of vis DLL)
  --vis-avs-dat <path>  Optional path to vis_avs.dat
  --out-dll <path>      Optional output plug-in DLL
  --preset <path>       Optional path to preset file
  --wav <path>          Path to WAV input (required)
  --width <pixels>      Output width (default: 640)
  --height <pixels>     Output height (default: 480)
  --fps <value>         Frames per second (default: 60)
  --frames <count>      Number of frames to render (default: 121)
  --out-dir <dir>       Output directory (required)
  --avi-out <filename>  Optional AVI output filename
  --png-step <value>    Interval between PNG dumps (default: 1)
  --hash-mode <mode>    Hashing mode: pixels|rolling (default: pixels)
  --help, -h            Show this help message
```

### Example Usage

In this example, visdriver.exe was placed in the Program Files (x86)\Winamp installation directory.
Take note of the effect of the --runtime-dir option on the other options search paths, it's not possible
to omit this value as it is automatically set to the path where `vis_avs.dat` is located. It's just 
a small annoyance.

```
.\visdriver.exe generate-verification-data`
 --runtime-dir .\Plugins\`
 --vis-avs-dat .\vis_avs.dat`
 --vis-dll .\vis_avs.dll`
 --out-dll .\out_wave.dll`
 --preset .\tests\data\phase1\color_mod.avs`
 --wav .\Plugins\tests\data\electronic.wav`
 --out-dir .\tests\out\`
 --avi-out test.avi
 --width 1000 --height 600
```

### CLI Output
The console output has been enhanced to provide useful information about the state of the program's operation:

```
PS C:\Program Files (x86)\Winamp> .\visdriver.exe generate-verification-data --runtime-dir .\Plugins\ --vis-avs-dat .\vis_avs.dat --vis-dll .\vis_avs.dll --out-dll .\out_wave.dll --preset .\tests\data\extended_color_mod.avs --wav .\Plugins\tests\data\test.wav --out-dir .\tests\out\
generate-verification-data options:
  Visualizer DLL:        C:\Program Files (x86)\Winamp\Plugins\vis_avs.dll
  Runtime directory:     C:\Program Files (x86)\Winamp\Plugins\
  vis_avs.dat:           C:\Program Files (x86)\Winamp\Plugins\vis_avs.dat
  Output plug-in DLL:    C:\Program Files (x86)\Winamp\Plugins\out_wave.dll
  Preset:                C:\Program Files (x86)\Winamp\Plugins\tests\data\extended_color_mod.avs
  WAV input:             C:\Program Files (x86)\Winamp\Plugins\tests\data\test.wav
  Output size (WxH):     640x480
  Frame rate:            60 fps
  Frame count:           121
  Output directory:      C:\Program Files (x86)\Winamp\tests\out\
  AVI output filename:   (not set)
  PNG step:              1
  Hash mode:             pixels
  Trace log filename:    avs_debug_trace.log
  Diagnostics fallback:  enabled
Runtime directory ready: C:\Program Files (x86)\Winamp\Plugins\
  entry: avs
  entry: CDDBControlWinamp.dll
  entry: CDDBUIWinamp.dll
  ...
vis_avs.dat: C:\Program Files (x86)\Winamp\Plugins\vis_avs.dat
  first 16 bytes hex: 4e 75 6c 6c 73 6f 66 74 20 41 56 53 20 50 72 65 ...
  text sample: Nullsoft AVS Preset 0.2.........
Preset: C:\Program Files (x86)\Winamp\Plugins\tests\data\extended_color_mod.avs
  first 16 bytes hex: 4e 75 6c 6c 73 6f 66 74 20 41 56 53 20 50 72 65 ...
  text sample: Nullsoft AVS Preset 0.2.....$...
Output plug-in probe: C:\Program Files (x86)\Winamp\Plugins\out_wave.dll
  description: waveOut output v2.0.2a
  version: 0x10
WAV ok: 44100 Hz, 2 ch, 88200 samples
```
And you'll find an `avs_debug_trace.log` file at the directory specify in `Output Directory`. This file's output should look a bit like this:

```
2024-06-15 21:10:30.101 [tid=4242] | == Debug trace initialized ==
2024-06-15 21:10:30.102 [tid=4242] | Debug trace logging to C:\\capture\\avs_debug_trace.log
2024-06-15 21:10:30.104 [tid=4242] | Render options: 800x600 @ 60 FPS for 240 frames
2024-06-15 21:10:30.105 [tid=4242] | Runtime directory: C:\\Program Files\\Winamp
2024-06-15 21:10:30.106 [tid=4242] | vis_avs.dat path: C:\\Program Files\\Winamp\\Plugins\\vis_avs.dat
2024-06-15 21:10:30.107 [tid=4242] | Preset path: D:\\avs-presets\\milkdrops\\aurora.avs
2024-06-15 21:10:30.109 [tid=4242] | Loaded WAV 'D:\\audio\\fixtures\\demo.wav' (529200 samples)
2024-06-15 21:10:30.110 [tid=4242] | SetCurrentDirectory to C:\\Program Files\\Winamp
2024-06-15 21:10:30.112 [tid=4242] | Loaded vis module from C:\\Program Files\\Winamp\\Plugins\\vis_avs.dll (module=0x10000000)
2024-06-15 21:10:30.113 [tid=4242] | Installed 25 debug hooks for C:\\Program Files\\Winamp\\Plugins\\vis_avs.dll (module=0x10000000)
2024-06-15 21:10:30.113 [tid=4242] |   hook: user32.dll!CreateWindowExW
2024-06-15 21:10:30.114 [tid=4242] |   hook: user32.dll!CreateWindowExA
2024-06-15 21:10:30.114 [tid=4242] |   hook: user32.dll!DestroyWindow
2024-06-15 21:10:30.114 [tid=4242] |   hook: user32.dll!GetDC
2024-06-15 21:10:30.114 [tid=4242] |   hook: user32.dll!GetDCEx
2024-06-15 21:10:30.114 [tid=4242] |   hook: user32.dll!GetWindowDC
2024-06-15 21:10:30.114 [tid=4242] |   hook: user32.dll!ReleaseDC
2024-06-15 21:10:30.115 [tid=4242] |   hook: user32.dll!BeginPaint
2024-06-15 21:10:30.115 [tid=4242] |   hook: user32.dll!EndPaint
2024-06-15 21:10:30.115 [tid=4242] |   hook: gdi32.dll!CreateCompatibleDC
2024-06-15 21:10:30.115 [tid=4242] |   hook: gdi32.dll!DeleteDC
2024-06-15 21:10:30.115 [tid=4242] |   hook: gdi32.dll!CreateCompatibleBitmap
2024-06-15 21:10:30.115 [tid=4242] |   hook: gdi32.dll!CreateDIBSection
2024-06-15 21:10:30.115 [tid=4242] |   hook: gdi32.dll!DeleteObject
2024-06-15 21:10:30.115 [tid=4242] |   hook: gdi32.dll!SelectObject
2024-06-15 21:10:30.116 [tid=4242] |   hook: gdi32.dll!BitBlt
2024-06-15 21:10:30.116 [tid=4242] |   hook: gdi32.dll!StretchBlt
2024-06-15 21:10:30.116 [tid=4242] |   hook: gdi32.dll!PatBlt
2024-06-15 21:10:30.116 [tid=4242] |   hook: gdi32.dll!SwapBuffers
2024-06-15 21:10:30.116 [tid=4242] |   hook: gdi32.dll!ChoosePixelFormat
2024-06-15 21:10:30.116 [tid=4242] |   hook: gdi32.dll!SetPixelFormat
2024-06-15 21:10:30.117 [tid=4242] |   hook: gdi32.dll!DescribePixelFormat
2024-06-15 21:10:30.117 [tid=4242] |   hook: opengl32.dll!wglCreateContext
2024-06-15 21:10:30.117 [tid=4242] |   hook: opengl32.dll!wglMakeCurrent
2024-06-15 21:10:30.117 [tid=4242] |   hook: opengl32.dll!wglDeleteContext
2024-06-15 21:10:30.122 [tid=4242] | CreateHiddenParentWindow width=800 height=600
2024-06-15 21:10:30.123 [tid=4242] | CreateWindowExW class=visdriver avs parent name=visdriver avs parent ex=0x00000080 style=0x00CF0000 parent=0x00000000
2024-06-15 21:10:30.123 [tid=4242] | CreateWindowExW -> 0x00020102
2024-06-15 21:10:30.125 [tid=4242] | CreateChildWindow parent=0x00020102 width=800 height=600
2024-06-15 21:10:30.126 [tid=4242] | CreateWindowExW class=visdriver avs child name=visdriver avs child ex=0x00000000 style=0x56000000 parent=0x00020102
2024-06-15 21:10:30.126 [tid=4242] | CreateWindowExW -> 0x00030106
2024-06-15 21:10:30.127 [tid=4242] | CreateWindowExA class=avs_plugin name=avs plugin window ex=0x00000000 style=0x50000000 parent=0x00030106
2024-06-15 21:10:30.127 [tid=4242] | CreateWindowExA -> 0x00040108
2024-06-15 21:10:30.130 [tid=4242] | GetDC(0x00030106) -> 0x0005010A
2024-06-15 21:10:30.132 [tid=4242] | GetDCEx(0x00030106, hrgn=0x00000000, flags=0x00000001) -> 0x0006010C
2024-06-15 21:10:30.133 [tid=4242] | GetWindowDC(0x00030106) -> 0x0007010E
2024-06-15 21:10:30.134 [tid=4242] | ReleaseDC(0x00030106, 0x0005010A) -> 1
2024-06-15 21:10:30.135 [tid=4242] | BeginPaint(0x00030106) -> 0x00080110
2024-06-15 21:10:30.136 [tid=4242] | EndPaint(0x00030106)
2024-06-15 21:10:30.137 [tid=4242] | CreateCompatibleDC(0x0007010E) -> 0x00090112
2024-06-15 21:10:30.138 [tid=4242] | CreateCompatibleBitmap(dc=0x00090112, 800x600) -> 0x000A0114
2024-06-15 21:10:30.139 [tid=4242] | CreateDIBSection(dc=0x00090112, usage=0) -> 0x000B0116
2024-06-15 21:10:30.140 [tid=4242] | SelectObject(dc=0x00090112, obj=HBITMAP 0x000A0114) -> HBITMAP 0x00000000
2024-06-15 21:10:30.141 [tid=4242] | BitBlt(dest=HDC 0x0007010E, src=HDC 0x00090112, size=800x600, rop=0x00CC0020)
2024-06-15 21:10:30.142 [tid=4242] | StretchBlt(dest=HDC 0x0007010E, src=HDC 0x00090112, size=800x600 -> 1024x768, rop=0x00CC0020)
2024-06-15 21:10:30.143 [tid=4242] | PatBlt(dc=HDC 0x00090112, x=0, y=0, size=800x600, rop=0x00F00021)
2024-06-15 21:10:30.144 [tid=4242] | ChoosePixelFormat(HDC 0x0007010E)
2024-06-15 21:10:30.145 [tid=4242] | DescribePixelFormat(dc=HDC 0x0007010E, format=32, bytes=52)
2024-06-15 21:10:30.145 [tid=4242] | DescribePixelFormat -> 32
2024-06-15 21:10:30.146 [tid=4242] | SetPixelFormat(HDC 0x0007010E, 32)
2024-06-15 21:10:30.147 [tid=4242] | wglCreateContext(HDC 0x0007010E)
2024-06-15 21:10:30.147 [tid=4242] | wglCreateContext -> HGLRC 0x000C0118
2024-06-15 21:10:30.148 [tid=4242] | wglMakeCurrent(dc=HDC 0x0007010E, ctx=HGLRC 0x000C0118)
2024-06-15 21:10:30.149 [tid=4242] | SwapBuffers(HDC 0x0007010E)
2024-06-15 21:10:30.150 [tid=4242] | DeleteObject(HBITMAP 0x000B0116)
2024-06-15 21:10:30.151 [tid=4242] | DeleteDC(0x00090112)
2024-06-15 21:10:30.152 [tid=4242] | CreateCompatibleDC(0x00000000) -> 0x000D011A
2024-06-15 21:10:30.153 [tid=4242] | CreateCompatibleBitmap(dc=0x000D011A, 800x600) -> 0x000E011C
2024-06-15 21:10:30.154 [tid=4242] | SelectObject(dc=0x000D011A, obj=HBITMAP 0x000E011C) -> HBITMAP 0x00000000
2024-06-15 21:10:30.155 [tid=4242] | PatBlt(dc=HDC 0x000D011A, x=0, y=0, size=800x600, rop=0x00F00021)
2024-06-15 21:10:30.156 [tid=4242] | BitBlt(dest=HDC 0x0007010E, src=HDC 0x000D011A, size=800x600, rop=0x00CC0020)
2024-06-15 21:10:30.157 [tid=4242] | ReleaseDC(0x00030106, 0x0007010E) -> 1
2024-06-15 21:10:30.158 [tid=4242] | Frame 0: begin
2024-06-15 21:10:30.158 [tid=4242] | Frame 0: waveform start=0 hop=1
2024-06-15 21:10:30.159 [tid=4242] | Frame 0: Render returned 0
2024-06-15 21:10:30.160 [tid=4242] | Frame 0: captured from window DC
2024-06-15 21:10:30.161 [tid=4242] | Frame 0: end
2024-06-15 21:10:30.261 [tid=4242] | Frame 239: begin
2024-06-15 21:10:30.262 [tid=4242] | Frame 239: waveform start=21168 hop=1
2024-06-15 21:10:30.263 [tid=4242] | Frame 239: Render returned 0
2024-06-15 21:10:30.264 [tid=4242] | Frame 239: captured from window DC
2024-06-15 21:10:30.265 [tid=4242] | Frame 239: end
2024-06-15 21:10:30.266 [tid=4242] | Render completed successfully
2024-06-15 21:10:30.267 [tid=4242] | wglDeleteContext(HGLRC 0x000C0118)
2024-06-15 21:10:30.268 [tid=4242] | DestroyWindow(0x00040108)
2024-06-15 21:10:30.269 [tid=4242] | DestroyWindow(0x00030106)
2024-06-15 21:10:30.270 [tid=4242] | DestroyWindow(0x00020102)
2024-06-15 21:10:30.271 [tid=4242] | == Debug trace shutdown ==
```

# How to Force Fullscreen Visualization into a Window

If you would like to force a fullscreen vis plugin into using a Window, there are two options:
- a) Wine's built-in [virtual desktop](https://wiki.winehq.org/FAQ#How_do_I_get_Wine_to_launch_an_application_in_a_virtual_desktop.3F) feature
- b) Using [Xephyr](https://en.wikipedia.org/wiki/Xephyr) for a quick way to a nested Xorg server,
     that your distro has already packaged.

For Wine's [virtual desktop](https://wiki.winehq.org/FAQ#How_do_I_get_Wine_to_launch_an_application_in_a_virtual_desktop.3F) feature, this wrapper should do:
```bash
#! /usr/bin/env bash
exec wine explorer /desktop=visdriver,1024x768 ./build/visdriver.exe "$@"
```

For Xephyr, a wrapper script like this should do:
```bash
#! /usr/bin/env bash
set -x -e -u
NESTED_DISPLAY=:1
Xephyr -screen 1024x768 "${NESTED_DISPLAY}" &
xorg_pid=$!
kill_xorg() { kill -2 "${xorg_pid}"; }
trap kill_xorg EXIT
export DISPLAY=${NESTED_DISPLAY}

wine ./build/visdriver.exe "$@"
```

If you need help getting that set up, please reach out.


# Known Limitations

Please expect some rough edges, and potentially even crashes with some plug-ins.

In particular, known limitations are:
- Waveform/spectrum needs 16bit stereo samples input, at the moment.
- Unicode in- and output plug-ins are yet to be supported.
- `in_linein.dll` (SHA1 `7ab08fcc5bc9ebfcc9a8e3d729fadf2cb05e173a`)
  of Winamp 5.66 crashes right after loading for an unknown reason.

---
[Sebastian Pipping](https://github.com/hartwork), Berlin, 2023
