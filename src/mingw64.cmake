# mingw64.cmake — cross-compile for Windows x86-64 from WSL/Linux
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=../mingw64.cmake -B build -S .
#
# Requires: sudo apt install gcc-mingw-w64-x86-64

set(CMAKE_SYSTEM_NAME   Windows)
set(CMAKE_SYSTEM_VERSION 10)

set(CMAKE_C_COMPILER    x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER  x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER   x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
