cmake_minimum_required(VERSION 3.20)
project(osm_import CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    src/Replicator.cpp
    src/AirportsLoader.cpp
    src/FAAObstacleLoader.cpp
    src/WMMLoader.cpp
    src/AirspaceLoader.cpp
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
