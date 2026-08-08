# Cross-compile toolchain for regional_install.exe (task #57 -- see that
# task's plan for full context). Only regional_install targets Windows;
# every other binary in this project stays Linux-only, built the normal
# way via the top-level CMakeLists.txt with no toolchain file at all.
#
# Usage (from the repo root, with vcpkg already bootstrapped and
# `vcpkg install libpqxx:x64-mingw-static` already run):
#
#   cmake -B build-win \
#     -DCMAKE_TOOLCHAIN_FILE=<vcpkg root>/scripts/buildsystems/vcpkg.cmake \
#     -DVCPKG_TARGET_TRIPLET=x64-mingw-static \
#     -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$(pwd)/cmake/mingw-w64-toolchain.cmake
#   cmake --build build-win --target regional_install
#
# This file is loaded BY vcpkg's own toolchain file (via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE), not passed directly as
# CMAKE_TOOLCHAIN_FILE -- that's what lets find_package(libpqxx CONFIG)
# resolve against vcpkg's x64-mingw-static packages while still cross-
# compiling with the system's MinGW-w64 toolchain instead of vcpkg's own
# (which doesn't provide a MinGW triplet compiler).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Package-manager-installed MinGW-w64 (POSIX threading variant -- required
# for full std::thread/std::mutex support; the "win32" threading variant
# historically has incomplete C++11 threading support). Un-suffixed names
# resolve to -posix via update-alternatives once that package is installed.
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Deliberately NOT setting CMAKE_FIND_ROOT_PATH/CMAKE_FIND_ROOT_PATH_MODE_*
# here -- vcpkg's own toolchain.cmake (which chain-loads this file, not the
# other way around) manages that itself so find_package(... CONFIG) can
# resolve vcpkg-installed packages; a hand-set ONLY-mode root path here
# ended up shadowing that and made libpqxx's config file unfindable.
