cmake_minimum_required(VERSION 3.20)
project(osm_import CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    # ---- Windows/MinGW cross-compile: regional_install only ----
    # Every other binary in this project (osm_import, regional_db_export,
    # the standalone loaders, etc.) is Linux-only and stays on the pkg-
    # config path below -- see cmake/mingw-w64-toolchain.cmake for how to
    # actually invoke this branch (needs vcpkg + that toolchain file, not
    # a plain `cmake -B build` the way the Linux build works). See task
    # #57's plan for why regional_install specifically is the one binary
    # ported: it's the one piece a customer's Windows machine needs to run
    # (installing a region bundle into a Postgres DB that stays on Linux),
    # and it already depends on nothing but pqxx/libpq -- no PROJ, no
    # Boost.Geometry, unlike the export-side tooling that stays Linux-only.
    # Resolved directly rather than via find_package(libpqxx CONFIG) +
    # libpqxx::pqxx: that imported target's own transitively-expanded
    # deps (libpq, libpgcommon, ...) land wherever CMake's generator
    # places them in the actual link line -- not necessarily adjacent to
    # each other or in dependency order -- which breaks a plain static
    # linker's single left-to-right archive scan (undefined symbols in
    # one archive aren't retried against an earlier-scanned archive that
    # would have resolved them). Explicitly finding and ordering every
    # archive ourselves sidesteps that entirely: everything below is a
    # plain path/name in a list, in the exact order given, no CMake-side
    # reordering to fight.
    find_library(PQXX_LIB      pqxx      REQUIRED)
    find_library(PQ_WIN_LIB    pq        REQUIRED)
    find_library(PGCOMMON_LIB  pgcommon  REQUIRED)
    find_library(PGPORT_LIB    pgport    REQUIRED)
    find_library(SSL_LIB       ssl       REQUIRED)
    find_library(CRYPTO_LIB    crypto    REQUIRED)
    find_path(PQXX_INCLUDE_DIR pqxx/pqxx REQUIRED)
    find_path(PQ_WIN_INCLUDE_DIR libpq-fe.h REQUIRED)

    add_executable(regional_install
        src/regional_install.cpp
        src/NavDB.cpp
        src/RegionalNodeMap.cpp
    )
    target_include_directories(regional_install PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${PQXX_INCLUDE_DIR}
        ${PQ_WIN_INCLUDE_DIR}
    )
    # --start-group/--end-group: pqxx/pq/pgcommon/pgport/ssl/crypto have
    # real circular-ish cross-references at the archive level (pgcommon
    # needs pgport, pq needs pgcommon, pqxx needs pq, ...) -- grouping
    # makes ld loop until everything resolves instead of relying on a
    # single left-to-right pass matching dependency order exactly.
    target_link_libraries(regional_install PRIVATE
        -Wl,--start-group
        ${PQXX_LIB} ${PQ_WIN_LIB} ${PGCOMMON_LIB} ${PGPORT_LIB} ${SSL_LIB} ${CRYPTO_LIB}
        -Wl,--end-group
        # libpq's real Windows system dependencies (winsock, crypto/LDAP
        # for SSPI/GSSAPI auth, registry/UI libs pulled in by its Win32
        # port layer) -- same list vcpkg's own libpq.pc reports.
        secur32 crypt32 wldap32 wsock32 ws2_32
        kernel32 user32 gdi32 winspool shell32 ole32 oleaut32 uuid comdlg32 advapi32
    )
    # The x64-mingw-static triplet only governs how vcpkg builds its own
    # ports (libpqxx/libpq/openssl land as .a archives) -- it says nothing
    # about the compiler's own runtime. Without -static the .exe still
    # imports libgcc_s_seh-1.dll, libstdc++-6.dll and libwinpthread-1.dll
    # from the cross-compiler's lib dir, and dies at load time with
    # 0xC0000135 (STATUS_DLL_NOT_FOUND) on any machine that doesn't have
    # MinGW-w64 installed -- i.e. every machine we ship to. Shipping those
    # three DLLs next to the .exe is not the fix: they're GCC-version-
    # locked, and a mismatched set fails just as hard at load time with
    # 0xC0000139 (STATUS_ENTRYPOINT_NOT_FOUND). One flag covers all three.
    target_link_options(regional_install PRIVATE -static)
    # -static above pulls in libwinpthread.a in full, which collides with
    # libpq.a's own bundled minimal pthreads-for-Windows shim
    # (pthread-win32.c.obj, built for non-MinGW Windows targets that don't
    # have a real pthreads implementation) -- both define pthread_mutex_lock
    # etc., which a dynamically-imported winpthread never surfaced (only a
    # DLL import stub, not full object inclusion, so no archive-level
    # symbol collision). Both are functionally equivalent standard pthread
    # semantics, so accepting the linker's first definition is safe here --
    # this is a known MinGW+libpq static-link interaction, not specific to
    # this project's own code.
    target_link_options(regional_install PRIVATE -Wl,--allow-multiple-definition)
    target_compile_features(regional_install PRIVATE cxx_std_20)

else()
# ---- Linux build: everything else (unchanged, just indented one level
# under this else() branch so the two build paths can share one file
# without a WIN32 cross-compile config ever reaching the pkg-config calls
# below, which would fail outright under MinGW) ----

find_package(Protobuf REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(PQXX REQUIRED libpqxx)
pkg_check_modules(PROJ REQUIRED proj>=6)

# libpq is pqxx's underlying C library; must be linked explicitly
# when pqxx was built as a static library
find_library(PQ_LIB pq REQUIRED)
find_library(LZ4_LIB lz4 REQUIRED)
find_library(EXPAT_LIB expat REQUIRED)
find_library(CURL_LIB curl REQUIRED)
find_path(EXPAT_INCLUDE expat.h)
find_path(LZ4_INCLUDE lz4frame.h)
find_path(PQ_INCLUDE libpq-fe.h PATH_SUFFIXES postgresql)

# ---- Main executable ----
add_executable(osm_import
    src/main.cpp
    src/OSMReader.cpp
    src/OSMMMap.cpp
    src/NavDB.cpp
    src/OSCReader.cpp
    src/DeltaApplier.cpp
    src/RegionalDeltaApplier.cpp
    src/Replicator.cpp
    src/AirportsLoader.cpp
    src/FAAObstacleLoader.cpp
    src/WMMLoader.cpp
    src/AirspaceLoader.cpp
    src/TerrainLoader.cpp
    src/PlanetDownloader.cpp
    src/GeoUtils.cpp
    src/RegionalNodeMap.cpp
    src/RegionPolygons.cpp
    src/RegionIndex.cpp
    src/WkbDecode.cpp
    proto/fileformat.proto
    proto/osmformat.proto
)

# Generate protobuf sources directly into the target
set(PROTO_OUT ${CMAKE_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${PROTO_OUT})

protobuf_generate(
    TARGET osm_import
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${PROTO_OUT}
)

target_include_directories(osm_import PRIVATE
    include
    ${PROTO_OUT}
    ${Protobuf_INCLUDE_DIRS}
    ${PQXX_INCLUDE_DIRS}
    ${PROJ_INCLUDE_DIRS}
    ${PQ_INCLUDE}
    ${LZ4_INCLUDE}
    ${EXPAT_INCLUDE}
)

target_link_libraries(osm_import PRIVATE
    ${Protobuf_LIBRARIES}
    ZLIB::ZLIB
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
    ${LZ4_LIB}
    ${EXPAT_LIB}
    ${PROJ_LIBRARIES}
    ${CURL_LIB}
)

target_compile_options(osm_import PRIVATE
    -O2 -g -Wall -Wextra -Wno-psabi
    ${PQXX_CFLAGS_OTHER}
    ${PROJ_CFLAGS_OTHER}
)

# Optional: jemalloc for better allocator perf under many threads
find_library(JEMALLOC_LIB jemalloc)
if(JEMALLOC_LIB)
    target_link_libraries(osm_import PRIVATE ${JEMALLOC_LIB})
    message(STATUS "Linking with jemalloc: ${JEMALLOC_LIB}")
endif()

# ---- airports_load ----

add_executable(airports_load
    src/airports_load.cpp
    src/AirportsLoader.cpp
)

target_include_directories(airports_load PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PQXX_INCLUDE_DIRS}
    ${PROJ_INCLUDE_DIRS}
)

target_link_libraries(airports_load PRIVATE
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
    ${PROJ_LIBRARIES}
)

target_compile_definitions(airports_load PRIVATE ${PQXX_CFLAGS_OTHER})

target_compile_options(airports_load PRIVATE -O2 -Wall -Wextra -Wno-psabi)
target_compile_features(airports_load PRIVATE cxx_std_20)

# ---- faa_obstacles_load ----

add_executable(faa_obstacles_load
    src/faa_obstacles_load.cpp
    src/FAAObstacleLoader.cpp
    src/GeoUtils.cpp
)

target_include_directories(faa_obstacles_load PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PQXX_INCLUDE_DIRS}
    ${PROJ_INCLUDE_DIRS}
    ${PQ_INCLUDE}
)

target_link_libraries(faa_obstacles_load PRIVATE
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
    ${PROJ_LIBRARIES}
    ${CURL_LIB}
)

target_compile_options(faa_obstacles_load PRIVATE -O2 -Wall -Wextra -Wno-psabi)
target_compile_features(faa_obstacles_load PRIVATE cxx_std_20)

# ---- terrain_load ----
# Shells out to raster2pgsql (part of the postgis package, already a
# dependency) for GeoTIFF loading/reprojection — no GDAL linkage needed here.

add_executable(terrain_load
    src/terrain_load.cpp
    src/TerrainLoader.cpp
)

target_include_directories(terrain_load PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PQXX_INCLUDE_DIRS}
)

target_link_libraries(terrain_load PRIVATE
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
    ${CURL_LIB}
)

target_compile_options(terrain_load PRIVATE -O2 -Wall -Wextra -Wno-psabi)
target_compile_features(terrain_load PRIVATE cxx_std_20)

# ---- regional_export ----
# Single-pass export of per-region node coordinate slices (nodes.dat ->
# <region>.nodes.dat) for the regional installer.

add_executable(regional_export
    src/regional_export.cpp
    src/OSMMMap.cpp
    src/RegionalNodeMap.cpp
    src/RegionPolygons.cpp
    src/RegionIndex.cpp
    src/WkbDecode.cpp
    src/GeoUtils.cpp
)

target_include_directories(regional_export PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PROJ_INCLUDE_DIRS}
    ${LZ4_INCLUDE}
)

target_link_libraries(regional_export PRIVATE
    ${LZ4_LIB}
    ${PROJ_LIBRARIES}
)

target_compile_options(regional_export PRIVATE -O2 -g -Wall -Wextra -Wno-psabi ${PROJ_CFLAGS_OTHER})
target_compile_features(regional_export PRIVATE cxx_std_20)

# ---- regional_db_export ----
# Exports every region-scoped Postgres table (paired with regional_export's
# nodes.dat output) for the regional installer bundle -- see
# regional_db_export.cpp's top-of-file comment for the two categories it
# handles (a single client-side pass for the 5 big parent+child table
# pairs, plain per-region `psql \copy` queries for everything else) and why
# they're one tool now instead of two (originally split as
# regional_table_export, merged back 2026-07-30: every real invocation
# needed both anyway, in a fixed order, so the split bought nothing beyond
# what an internal code path would have).

add_executable(regional_db_export
    src/regional_db_export.cpp
    src/RegionPolygons.cpp
    src/RegionIndex.cpp
    src/WkbDecode.cpp
    src/PgCopyBinary.cpp
    src/GeoUtils.cpp
)

target_include_directories(regional_db_export PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PQXX_INCLUDE_DIRS}
    ${PQ_INCLUDE}
    ${PROJ_INCLUDE_DIRS}
)

target_link_libraries(regional_db_export PRIVATE
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
    ${PROJ_LIBRARIES}
)

target_compile_options(regional_db_export PRIVATE -O2 -g -Wall -Wextra -Wno-psabi ${PQXX_CFLAGS_OTHER} ${PROJ_CFLAGS_OTHER})
target_compile_features(regional_db_export PRIVATE cxx_std_20)

# ---- regional_install ----
# Installs a <region>.gpsxdb.tar.gz bundle (regional_export + regional_db_export
# output, tarred/gzipped) into a target database, deduping against any
# previously-installed regions, and merges the region's node coordinates
# into the target's local nodes.dat. Shells out to `psql -f` for the actual
# staged-load script (needs a single session for TEMP TABLEs + \copy to
# share scope) and reuses NavDB::ensureSchema() for idempotent schema setup.

add_executable(regional_install
    src/regional_install.cpp
    src/NavDB.cpp
    src/RegionalNodeMap.cpp
)

target_include_directories(regional_install PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PQXX_INCLUDE_DIRS}
    ${PQ_INCLUDE}
)

target_link_libraries(regional_install PRIVATE
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
)

target_compile_options(regional_install PRIVATE -O2 -g -Wall -Wextra -Wno-psabi ${PQXX_CFLAGS_OTHER})
target_compile_features(regional_install PRIVATE cxx_std_20)

# ---- wmm_load ----
# Computes World Magnetic Model declination directly (no external tile
# source, no GDAL/raster2pgsql — the raster is built via plain PostGIS
# raster SQL calls, see WMMLoader.cpp).

add_executable(wmm_load
    src/wmm_load.cpp
    src/WMMLoader.cpp
)

target_include_directories(wmm_load PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PQXX_INCLUDE_DIRS}
)

target_link_libraries(wmm_load PRIVATE
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
)

target_compile_options(wmm_load PRIVATE -O2 -Wall -Wextra -Wno-psabi)
target_compile_features(wmm_load PRIVATE cxx_std_20)

# ---- airspace_load ----
# FAA Class Airspace / Special Use Airspace GeoJSON downloads, parsed with
# Boost.JSON (header-only here via boost/json/src.hpp in AirspaceLoader.cpp
# — no separate libboost_json to link).

add_executable(airspace_load
    src/airspace_load.cpp
    src/AirspaceLoader.cpp
    src/GeoUtils.cpp
)

target_include_directories(airspace_load PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PQXX_INCLUDE_DIRS}
    ${PROJ_INCLUDE_DIRS}
    ${PQ_INCLUDE}
)

target_link_libraries(airspace_load PRIVATE
    ${PQXX_LIBRARIES}
    ${PQ_LIB}
    ${PROJ_LIBRARIES}
    ${CURL_LIB}
)

target_compile_options(airspace_load PRIVATE -O2 -Wall -Wextra -Wno-psabi)
target_compile_features(airspace_load PRIVATE cxx_std_20)

endif() # WIN32
