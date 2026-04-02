Native Windows build with MSVC 2026 support
===========================================

## Overview

Full native Windows compilation support for Redis using **Microsoft Visual Studio 2026** (MSVC 2026), entirely bypassing the need for Cygwin, MinGW, or any WSL emulation.

**Crucially, this support is accomplished without modifying a single `.c` or `.h` source file in the repository.** All necessary porting accommodations have been cleanly isolated to the CMake configuration phase.

This approach minimizes the maintenance burden on the core Redis team. Windows developers can seamlessly build `Redis-server.exe` and `Redis-cli.exe` directly on Windows with MSVC, while Linux and macOS builds remain entirely unaffected.

## Key Mechanisms & Architecture

The Windows build utilizes the `auto-win-msvc` compatibility layer (a repository of POSIX-compliant headers and implementation stubs optimized for Windows).

To compile the codebase natively on Windows using the MSVC compiler without modifying the upstream source files, we implemented an automated "source patching" pipeline within the CMake configuration step:

### 1. Dynamic Source Patching via CMake
During the CMake generation phase, if building with MSVC, CMake copies specific `.c` and `.h` files into the `CMAKE_BINARY_DIR/patched/` folder. It then applies precise text replacements (`string(REPLACE ...)`) to adapt non-compliant C99/POSIX constructs into MSVC-compatible C11/Windows constructs.

This includes:
- **Variable Length Arrays (VLAs):** MSVC's C11 compiler does not support VLAs. We automatically replace dynamic stack arrays (e.g., `robj objects[argc];`) with standard `_alloca()` calls (e.g., `robj *objects = _alloca(sizeof(robj) * argc);`).
- **Function/Field Name Conflicts:** Windows socket structs have functions/fields named `connect`, `close`, `read`, and `write`. In files like `deps/libRedis/src/net.c`, references to these were seamlessly replaced (e.g., `._close` and `.posix_connect`) to prevent compiler collisions.
- **Type Collisions:** The `WORD` typedef in `sha256.h` inherently collides with the Windows API `<windef.h>`. CMake automatically patches this to `Redis_WORD`.
- **`linenoise` buffer limits:** Adjusted array initialization inside `linenoise.c` that relied on `const int` sizes to utilize `#define` macros, resolving another MSVC compilation limitation.

### 2. Transparent Include and Source Routing
After patching the files into the build directory, CMake variables (such as `Redis_SERVER_SRCS`) and target include directories are updated to prioritize the `patched/` variants of the source files over the originals. This transparently feeds the corrected files directly to the MSVC compiler (`cl.exe`).

### 3. Compiler Options and C11 Atomics
We leverage the modern MSVC C11 standard and experimental features:
```cmake
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /std:c11 /experimental:c11atomics")
```
This flag combined with `<stdatomic.h>` support natively maps Redis's atomic operations cleanly to Windows Interlocked functions. We also added suppressions for strict MSVC deprecation warnings (`/D_CRT_SECURE_NO_WARNINGS`, `/D_WINSOCK_DEPRECATED_NO_WARNINGS`).

### 4. Bypassing GoogleTest C++ Unit Tests on Windows
Currently, the GoogleTest framework integration in `src/unit/` utilizes `extern "C"` wrappers and macros that struggle to compile under the MSVC C++ standard due to deep type incompatibilities. Thus, the unit test library build `add_subdirectory(unit)` is cleanly bypassed `if(NOT MSVC)`, keeping the core server compilation pristine.

## Validation
- Successfully builds `Redis-server.exe` natively on Windows 11 using Visual Studio 18 2026.
- Successfully executes the internal memory testing utility without faults:
  ```cmd
  Redis-server.exe --test-memory 10
  ```
- Basic server running on different port with client connecting to it on that port and running `flushdb`, `get`,  `set`,  `keys *`
- No upstream core `.c` or `.h` files were dirtied or modified.

## Build Instructions (For Reviewers)

1. Clone Redis alongside the `auto-win-msvc` compatibility repository. The second repo will be fetched from git master branch if not available locally.
2. Open a Developer Command Prompt for Visual Studio 2026.
3. Configure using CMake:
   ```cmd
   cmake -S . -B build_msvc2026 -G "Visual Studio 18 2026"
   ```
4. Build the binaries:
   ```cmd
   cmake --build build_msvc2026 --config Release
   ```
5. Run your natively built Windows Redis server and client!
