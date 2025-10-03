You’re hitting the classic MinGW runtime deps. Two clean ways to fix it:

---

## Option A (recommended): statically link libgcc + libstdc++

This removes the need to ship `libgcc_s_dw2-1.dll` and `libstdc++-6.dll`.

In your top-level `CMakeLists.txt` (or the one that defines `visdriver`):

```cmake
# after add_executable(visdriver ...)

# Link libstdc++ and libgcc statically so those DLLs aren't needed at runtime
target_link_options(visdriver PRIVATE -static-libstdc++ -static-libgcc)

# Optional: also try to fully static link (pulls in libwinpthread, etc.)
# Beware: this can increase size and occasionally cause TLS/WSA quirks.
# target_link_options(visdriver PRIVATE -static)
```

Rebuild:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build
cmake --build build -j"$(nproc)" --verbose
```

Notes:

* If your code (or dependencies) use threads, you may still end up needing `libwinpthread-1.dll` unless you also go `-static` or explicitly link a static libwinpthread. Test with `ntldd`/`objdump` (below).

---

## Option B: ship the correct 32-bit DLLs next to the EXE

Keep dynamic linking, but copy the exact DLLs your 32-bit cross-compiler expects (watch the 32-bit vs 64-bit mismatch—your toolchain is `i686-…`, so you need **32-bit** DLLs).

Add this to your `CMakeLists.txt` after `add_executable(visdriver …)`:

```cmake
if(MINGW)
  # Query the compiler for the *matching* runtime DLL paths (avoids 64/32-bit mixups)
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++-6.dll
                  OUTPUT_VARIABLE LIBSTDCPP_DLL OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgcc_s_dw2-1.dll
                  OUTPUT_VARIABLE LIBGCC_DLL OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libwinpthread-1.dll
                  OUTPUT_VARIABLE PTHREAD_DLL OUTPUT_STRIP_TRAILING_WHITESPACE)

  add_custom_command(TARGET visdriver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LIBSTDCPP_DLL} ${LIBGCC_DLL} ${PTHREAD_DLL}
            $<TARGET_FILE_DIR:visdriver>)
endif()
```

Then run either:

```bash
# Run under Wine with the EXE folder on PATH
( cd build && WINEPATH="$PWD" wine visdriver.exe )
```

or just copy the three DLLs next to `visdriver.exe` on Windows.

---

## Why you saw `c000035a` and `c0000135`

* `c0000135` = DLL not found.
* `c000035a` commonly shows up when the **architecture doesn’t match** (e.g., you grabbed a 64-bit `libstdc++-6.dll` for a 32-bit EXE). Using `-print-file-name` from the **same** `i686-w64-mingw32-*` compiler guarantees you pick the right 32-bit DLLs.

---

## Quick dependency check

After building, verify imports:

```bash
# If you have ntldd:
ntldd build/visdriver.exe

# Or with binutils:
i686-w64-mingw32-objdump -p build/visdriver.exe | grep -i "DLL Name"
```

You should either see *no* `libgcc_s_dw2-1.dll` / `libstdc++-6.dll` (when statically linked), or see only 32-bit DLLs you plan to ship.

---

## Extra tips

* If you *meant* to build 64-bit, switch toolchain to `x86_64-w64-mingw32-*` and clean/reconfigure; the runtime then uses SEH (no `_dw2`).
* Keep `WIN32 ON`/`MINGW ON` as you have; they’re fine.
* If you still get a `libwinpthread-1.dll` missing error, either copy it (Option B) or add `-static` (full static) in Option A.

Pick A (simpler deployment) or B (ship DLLs). I’d start with A: it’s usually the least hassle for a small utility like `visdriver`.
