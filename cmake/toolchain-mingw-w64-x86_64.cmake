# Cross-compile RetComM for Windows x64 from Linux with mingw-w64.
#
#   cmake -S . -B build-windows -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x86_64.cmake
#
# Releases are built with MSVC + vcpkg (see .github/workflows/release.yml); this
# toolchain exists for local Windows *dev test* builds — see
# packaging/windows/build-dev-zip.sh, which drives it end to end.
#
# Override the triple/sysroot for a different mingw layout:
#   -DMINGW_TARGET_TRIPLE=x86_64-w64-mingw32 -DMINGW_SYSROOT=/usr/x86_64-w64-mingw32

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TARGET_TRIPLE "x86_64-w64-mingw32" CACHE STRING "mingw-w64 target triple")
set(MINGW_SYSROOT "/usr/${MINGW_TARGET_TRIPLE}" CACHE PATH "mingw-w64 sysroot")

set(CMAKE_C_COMPILER   "${MINGW_TARGET_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${MINGW_TARGET_TRIPLE}-g++")
set(CMAKE_RC_COMPILER  "${MINGW_TARGET_TRIPLE}-windres")
set(CMAKE_AR           "${MINGW_TARGET_TRIPLE}-ar")
set(CMAKE_RANLIB       "${MINGW_TARGET_TRIPLE}-ranlib")

# Prefer the cross pkg-config so .pc files come from the sysroot, not the host.
find_program(MINGW_PKG_CONFIG "${MINGW_TARGET_TRIPLE}-pkg-config")
if(MINGW_PKG_CONFIG)
    set(PKG_CONFIG_EXECUTABLE "${MINGW_PKG_CONFIG}" CACHE FILEPATH "" FORCE)
endif()

# Root paths searched for headers/libraries/config packages. CMAKE_PREFIX_PATH
# entries are re-rooted under CMAKE_FIND_ROOT_PATH when PACKAGE mode is ONLY, so
# an out-of-sysroot prefix (the cross-built SDL3) has to be a root itself.
set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_FIND_ROOT_PATH "${_prefix}")
endforeach()
list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)
# Programs (cmake, pkg-config, git) must come from the host; everything the
# build links or includes must come from the sysroot / an explicit prefix.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The dev zip ships app DLLs (SDL3, curl, …) but not a GCC runtime; static
# libgcc/libstdc++ keeps libgcc_s_seh-1.dll / libstdc++-6.dll out of the payload.
foreach(_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_INIT "-static-libgcc -static-libstdc++")
endforeach()
